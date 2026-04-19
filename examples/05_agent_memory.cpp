/// examples/05_agent_memory.cpp — Agent memory with llama.cpp server + celer-mem
/// Demonstrates:
///   - Persistent agent memory (RocksDB) vs ephemeral (in-memory)
///   - Conversation turns stored as structured keys: turn:<N>:{role,content}
///   - Context window assembly via prefix scan before each LLM call
///   - Summary compaction: compress old turns into a rolling summary
///   - HTTP client for llama.cpp server (/v1/chat/completions)
///   - Auto-downloads a small model (TinyLlama 1.1B Q4) if not present
///
/// NOTE: TinyLlama 1.1B is a small, dated model used here purely for demo
/// purposes (fast download, runs on any hardware). For real use, point
/// --model at a larger GGUF (Llama 3, Mistral, Qwen, etc.) and adjust
/// --host/--port to match your llama-server instance.
///
/// ─── Quick Start (Docker — easiest way if you're new to C++) ───
///
///   The example auto-downloads a TinyLlama 1.1B GGUF model on first run.
///   To serve it, the fastest path is Docker — no build tools needed:
///
///     mkdir -p models
///     docker run --rm -v ./models:/models -p 8080:8080
///       ghcr.io/ggml-org/llama.cpp:server
///       -m /models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
///       --port 8080 --host 0.0.0.0
///
///   Then in another terminal:
///     ./build/examples/05_agent_memory --memory
///
/// ─── Without Docker ───
///
///   llama-server -m models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf --port 8080
///
/// Build: make examples
/// Run:   ./build/examples/05_agent_memory --rocksdb   (persistent)
///        ./build/examples/05_agent_memory --memory     (ephemeral, no deps)

#include <celer/celer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// POSIX sockets for the HTTP client — no external deps
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

// ════════════════════════════════════════════════════════════════════
// Model auto-download — fetches TinyLlama GGUF if not already present
// ════════════════════════════════════════════════════════════════════

static constexpr const char* DEFAULT_MODEL_DIR  = "models";
static constexpr const char* DEFAULT_MODEL_FILE  = "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf";
static constexpr const char* MODEL_URL =
    "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/"
    "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf";

/// Ensure the model file exists at `path`. Downloads via curl if missing.
/// Returns true on success (file exists or was downloaded).
[[nodiscard]] static auto ensure_model(const std::filesystem::path& model_path) -> bool {
    if (std::filesystem::exists(model_path)) {
        std::cout << "Model found: " << model_path << "\n";
        return true;
    }

    std::cout << "Model not found at " << model_path << "\n"
              << "Downloading TinyLlama 1.1B (Q4_K_M, ~670 MB)...\n"
              << "  (this only happens once)\n";

    // Create directory if needed
    std::filesystem::create_directories(model_path.parent_path());

    // Download with curl — follows redirects, shows progress
    std::string cmd = "curl -L --progress-bar -o \"" +
                      model_path.string() + "\" \"" + MODEL_URL + "\"";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "Download failed (exit " << rc << "). Install curl or download manually:\n"
                  << "  curl -L -o " << model_path << " " << MODEL_URL << "\n";
        return false;
    }

    std::cout << "Download complete: " << model_path << "\n";
    return true;
}

// ════════════════════════════════════════════════════════════════════
// Minimal HTTP client — just enough for llama.cpp /v1/chat/completions
// ════════════════════════════════════════════════════════════════════

namespace http {

struct Response {
    int         status{0};
    std::string body;
};

/// POST json to host:port/path. Returns the response body.
[[nodiscard]] auto post(const std::string& host, int port,
                        const std::string& path, const std::string& json_body)
    -> celer::Result<Response>
{
    // Resolve host
    addrinfo hints{}, *res{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    auto port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        return std::unexpected(celer::Error{"HttpResolve", "cannot resolve " + host});
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return std::unexpected(celer::Error{"HttpSocket", "socket() failed"}); }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res); close(fd);
        return std::unexpected(celer::Error{"HttpConnect", "connect to " + host + ":" + port_str + " failed"});
    }
    freeaddrinfo(res);

    // Build HTTP/1.1 request
    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << json_body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << json_body;

    auto payload = req.str();
    if (send(fd, payload.data(), payload.size(), 0) < 0) {
        close(fd); return std::unexpected(celer::Error{"HttpSend", "send() failed"});
    }

    // Read response
    std::string raw;
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = recv(fd, buf.data(), buf.size(), 0)) > 0) {
        raw.append(buf.data(), static_cast<std::size_t>(n));
    }
    close(fd);

    // Parse status line
    Response resp;
    if (auto pos = raw.find(' '); pos != std::string::npos) {
        resp.status = std::stoi(raw.substr(pos + 1, 3));
    }
    // Body starts after \r\n\r\n
    if (auto pos = raw.find("\r\n\r\n"); pos != std::string::npos) {
        resp.body = raw.substr(pos + 4);
    }

    return resp;
}

} // namespace http

// ════════════════════════════════════════════════════════════════════
// Tiny JSON helpers — extract string fields without a JSON library
// ════════════════════════════════════════════════════════════════════

namespace json {

/// Extract the string value for "key" from a JSON blob. Handles escaped quotes.
[[nodiscard]] auto extract_string(const std::string& blob, const std::string& key) -> std::string {
    auto needle = "\"" + key + "\"";
    auto pos = blob.find(needle);
    if (pos == std::string::npos) return {};
    // Find the colon, then the opening quote
    pos = blob.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = blob.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos; // skip opening "
    std::string result;
    for (; pos < blob.size(); ++pos) {
        if (blob[pos] == '\\' && pos + 1 < blob.size()) {
            char esc = blob[pos + 1];
            if (esc == '"')       { result += '"'; ++pos; }
            else if (esc == 'n')  { result += '\n'; ++pos; }
            else if (esc == '\\') { result += '\\'; ++pos; }
            else if (esc == 't')  { result += '\t'; ++pos; }
            else                  { result += blob[pos]; }
        } else if (blob[pos] == '"') {
            break;
        } else {
            result += blob[pos];
        }
    }
    return result;
}

/// Escape a string for JSON embedding.
[[nodiscard]] auto escape(const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

} // namespace json

// ════════════════════════════════════════════════════════════════════
// InMemoryBackend — same as example 04, satisfies StorageBackend
// ════════════════════════════════════════════════════════════════════

class InMemoryBackend {
public:
    [[nodiscard]] static auto name() noexcept -> std::string_view { return "in_memory"; }

    [[nodiscard]] auto get(std::string_view key) -> celer::Result<std::optional<std::string>> {
        std::lock_guard lk(mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::optional<std::string>{std::nullopt};
        return std::optional<std::string>{it->second};
    }

    [[nodiscard]] auto put(std::string_view key, std::string_view value) -> celer::VoidResult {
        std::lock_guard lk(mu_);
        store_[std::string(key)] = std::string(value);
        return {};
    }

    [[nodiscard]] auto del(std::string_view key) -> celer::VoidResult {
        std::lock_guard lk(mu_);
        if (auto it = store_.find(key); it != store_.end()) store_.erase(it);
        return {};
    }

    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> celer::Result<std::vector<celer::KVPair>> {
        std::lock_guard lk(mu_);
        std::vector<celer::KVPair> results;
        for (auto it = store_.lower_bound(prefix); it != store_.end() && it->first.starts_with(prefix); ++it) {
            results.push_back(celer::KVPair{it->first, it->second});
        }
        return results;
    }

    [[nodiscard]] auto batch(std::span<const celer::BatchOp> ops) -> celer::VoidResult {
        std::lock_guard lk(mu_);
        for (const auto& op : ops) {
            if (op.kind == celer::BatchOp::Kind::put && op.value) store_[op.key] = *op.value;
            else store_.erase(op.key);
        }
        return {};
    }

    [[nodiscard]] auto compact() -> celer::VoidResult { return {}; }

    [[nodiscard]] auto foreach_scan(std::string_view prefix, celer::ScanVisitor visitor, void* ctx) -> celer::VoidResult {
        std::lock_guard lk(mu_);
        for (auto it = store_.lower_bound(prefix); it != store_.end() && it->first.starts_with(prefix); ++it) {
            visitor(ctx, it->first, it->second);
        }
        return {};
    }

    // ── Streaming stubs (RFC-002) — materialize via get/put/prefix_scan ──

    [[nodiscard]] auto stream_get(std::string_view key) -> celer::Result<celer::StreamHandle<char>> {
        auto r = get(key);
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return celer::stream::empty<char>();
        return celer::stream::from_string(std::move(r->value()));
    }

    [[nodiscard]] auto stream_put(std::string_view key, celer::StreamHandle<char> input) -> celer::VoidResult {
        auto collected = celer::stream::collect_string(input);
        if (!collected) return std::unexpected(collected.error());
        return put(key, *collected);
    }

    [[nodiscard]] auto stream_scan(std::string_view prefix) -> celer::Result<celer::StreamHandle<celer::KVPair>> {
        auto r = prefix_scan(prefix);
        if (!r) return std::unexpected(r.error());
        return celer::stream::from_vector(std::move(*r));
    }

private:
    std::map<std::string, std::string, std::less<>> store_;
    std::mutex mu_;
};

static_assert(celer::StorageBackend<InMemoryBackend>);

// ════════════════════════════════════════════════════════════════════
// Agent — conversation memory backed by celer-mem
// ════════════════════════════════════════════════════════════════════

struct AgentConfig {
    std::string llm_host  = "127.0.0.1";
    int         llm_port  = 8080;
    std::string model     = "default";
    int         max_ctx_turns = 20;     // max turns to feed as context
    int         summary_threshold = 30; // compress after this many turns
};

class Agent {
public:
    Agent(celer::Store store, AgentConfig cfg)
        : store_(std::move(store)), cfg_(std::move(cfg)) {}

    /// Run the main chat loop. Returns when user types /quit.
    auto run() -> void {
        std::cout << "\n🦎 Agent ready. Type /quit to exit, /history to dump, /summary to compact.\n\n";

        // Restore turn counter from previous session (if persistent backend)
        restore_turn_counter();

        std::string line;
        while (true) {
            std::cout << "you> ";
            if (!std::getline(std::cin, line) || line == "/quit") break;

            if (line.empty()) continue;
            if (line == "/history") { dump_history(); continue; }
            if (line == "/summary") { compact_history(); continue; }

            // 1. Store user turn
            store_turn("user", line);

            // 2. Build context window from memory
            auto messages = build_context();

            // 3. Call LLM
            auto reply = call_llm(messages);
            if (!reply) {
                std::cerr << "  [llm error: " << reply.error().message << "]\n";
                continue;
            }

            // 4. Store assistant turn
            store_turn("assistant", *reply);
            std::cout << "agent> " << *reply << "\n\n";

            // 5. Auto-compact if too many raw turns
            if (turn_counter_ > cfg_.summary_threshold) {
                std::cout << "  [auto-compacting " << turn_counter_ << " turns...]\n";
                compact_history();
            }
        }
        std::cout << "Goodbye.\n";
    }

private:
    celer::Store store_;
    AgentConfig  cfg_;
    int          turn_counter_{0};

    /// Storage layout:
    ///   agent/turns   — "turn:NNNN:role" → role, "turn:NNNN:content" → text
    ///   agent/meta    — "turn_counter" → N, "summary" → compressed history

    auto turns_tbl() -> celer::TableRef { return *store_.db("agent")->table("turns"); }
    auto meta_tbl()  -> celer::TableRef { return *store_.db("agent")->table("meta"); }

    auto turn_key(int n, const char* field) -> std::string {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "turn:%04d:%s", n, field);
        return buf;
    }

    void restore_turn_counter() {
        auto got = meta_tbl().get_raw("turn_counter");
        if (got && got->has_value()) {
            turn_counter_ = std::stoi(got->value());
            std::cout << "  [restored " << turn_counter_ << " turns from previous session]\n";
        }
    }

    void store_turn(const std::string& role, const std::string& content) {
        auto t = turns_tbl();
        (void)t.put_raw(turn_key(turn_counter_, "role"), role);
        (void)t.put_raw(turn_key(turn_counter_, "content"), content);
        ++turn_counter_;
        (void)meta_tbl().put_raw("turn_counter", std::to_string(turn_counter_));
    }

    /// Assemble the last N turns + any rolling summary into a messages array.
    [[nodiscard]] auto build_context() -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> messages;

        // System prompt — minimal for small models, longer for bigger ones
        messages.emplace_back("system",
            "You are a helpful chat assistant. Be brief and direct. "
            "Answer questions using the conversation history provided.");

        // Inject rolling summary if it exists
        auto sum = meta_tbl().get_raw("summary");
        if (sum && sum->has_value() && !sum->value().empty()) {
            messages.emplace_back("system",
                "[Memory summary of earlier conversation]: " + sum->value());
        }

        // Last N turns
        int start = std::max(0, turn_counter_ - cfg_.max_ctx_turns);
        for (int i = start; i < turn_counter_; ++i) {
            auto role    = turns_tbl().get_raw(turn_key(i, "role"));
            auto content = turns_tbl().get_raw(turn_key(i, "content"));
            if (role && role->has_value() && content && content->has_value()) {
                messages.emplace_back(role->value(), content->value());
            }
        }

        return messages;
    }

    /// Call llama.cpp server /completion with manually applied chat template.
    /// Forces TinyLlama/Zephyr template: <|role|>\ncontent</s>\n
    [[nodiscard]] auto call_llm(const std::vector<std::pair<std::string, std::string>>& messages)
        -> celer::Result<std::string>
    {
        // Build the raw prompt with explicit chat template
        std::ostringstream prompt;
        for (const auto& [role, content] : messages) {
            prompt << "<|" << role << "|>\n" << content << "</s>\n";
        }
        prompt << "<|assistant|>\n";

        // Build JSON body for /completion (raw prompt, no server-side template)
        std::ostringstream body;
        body << R"({"prompt":")" << json::escape(prompt.str()) << R"(")"
             << R"(,"temperature":0.4,"n_predict":150)"
             << R"(,"top_p":0.85,"top_k":30)"
             << R"(,"repeat_penalty":1.3,"frequency_penalty":0.3,"presence_penalty":0.3)"
             << R"(,"stop":["</s>","<|user|>","<|system|>","\nuser>","\nyou>"])"
             << "}";

        auto resp = http::post(cfg_.llm_host, cfg_.llm_port, "/completion", body.str());
        if (!resp) return std::unexpected(resp.error());

        if (resp->status != 200) {
            return std::unexpected(celer::Error{"LLM",
                "HTTP " + std::to_string(resp->status) + ": " + resp->body.substr(0, 200)});
        }

        // Extract content from response
        auto content = json::extract_string(resp->body, "content");
        if (content.empty()) {
            return std::unexpected(celer::Error{"LLM", "no content in response"});
        }
        return content;
    }

    /// Ask the LLM to summarize all turns, then delete them and store the summary.
    void compact_history() {
        if (turn_counter_ == 0) {
            std::cout << "  [nothing to compact]\n";
            return;
        }

        // Build all turns as text
        std::ostringstream all;
        for (int i = 0; i < turn_counter_; ++i) {
            auto role    = turns_tbl().get_raw(turn_key(i, "role"));
            auto content = turns_tbl().get_raw(turn_key(i, "content"));
            if (role && role->has_value() && content && content->has_value()) {
                all << role->value() << ": " << content->value() << "\n";
            }
        }

        // Existing summary
        auto old_sum = meta_tbl().get_raw("summary");
        std::string existing;
        if (old_sum && old_sum->has_value()) existing = old_sum->value();

        // Ask LLM to summarize — same tight prompt style
        std::vector<std::pair<std::string, std::string>> msg{
            {"system", "Summarize the conversation below into one short paragraph (3-5 sentences). "
                       "Keep only key facts, names, decisions, and action items. No filler."},
        };
        if (!existing.empty()) {
            msg.emplace_back("user", "Previous summary:\n" + existing);
        }
        msg.emplace_back("user", "New conversation:\n" + all.str());

        auto summary = call_llm(msg);
        if (!summary) {
            std::cerr << "  [compact failed: " << summary.error().message << "]\n";
            return;
        }

        // Delete all individual turns
        for (int i = 0; i < turn_counter_; ++i) {
            (void)turns_tbl().del(turn_key(i, "role"));
            (void)turns_tbl().del(turn_key(i, "content"));
        }

        // Store summary and reset counter
        (void)meta_tbl().put_raw("summary", *summary);
        turn_counter_ = 0;
        (void)meta_tbl().put_raw("turn_counter", "0");

        std::cout << "  [compacted to summary: " << summary->size() << " chars]\n";
    }

    void dump_history() {
        auto sum = meta_tbl().get_raw("summary");
        if (sum && sum->has_value() && !sum->value().empty()) {
            std::cout << "  [summary]: " << sum->value() << "\n";
        }
        for (int i = 0; i < turn_counter_; ++i) {
            auto role    = turns_tbl().get_raw(turn_key(i, "role"));
            auto content = turns_tbl().get_raw(turn_key(i, "content"));
            if (role && role->has_value() && content && content->has_value()) {
                std::cout << "  [" << role->value() << "]: " << content->value() << "\n";
            }
        }
        if (turn_counter_ == 0) {
            std::cout << "  (no turns stored)\n";
        }
    }
};

// ════════════════════════════════════════════════════════════════════
// main — select backend from CLI flags
// ════════════════════════════════════════════════════════════════════

auto make_memory_factory() -> celer::BackendFactory {
    return [](std::string_view, std::string_view) -> celer::Result<celer::BackendHandle> {
        return celer::make_backend_handle<InMemoryBackend>(new InMemoryBackend());
    };
}

int main(int argc, char* argv[]) {
    std::string mode = "--memory";  // default: in-memory
    std::string db_path = "/tmp/celer_agent";
    std::string host = "127.0.0.1";
    int port = 8080;
    std::filesystem::path model_path =
        std::filesystem::path(DEFAULT_MODEL_DIR) / DEFAULT_MODEL_FILE;
    bool skip_download = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--rocksdb")       mode = "--rocksdb";
        else if (arg == "--memory")   mode = "--memory";
        else if (arg == "--path" && i + 1 < argc)  db_path = argv[++i];
        else if (arg == "--host" && i + 1 < argc)  host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)  port = std::stoi(argv[++i]);
        else if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--no-download") skip_download = true;
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--rocksdb|--memory] [--path DIR] [--host H] [--port P] [--model FILE]\n"
                      << "  --rocksdb     Persist memory to disk (survives restarts)\n"
                      << "  --memory      Ephemeral in-memory (default, zero deps)\n"
                      << "  --path DIR    RocksDB storage path (default: /tmp/celer_agent)\n"
                      << "  --host H      llama-server host (default: 127.0.0.1)\n"
                      << "  --port P      llama-server port (default: 8080)\n"
                      << "  --model FILE  Path to GGUF model (default: models/tinyllama-...gguf)\n"
                      << "  --no-download Skip automatic model download\n";
            return 0;
        }
    }

    // Ensure the model is available (auto-downloads on first run)
    if (!skip_download && !ensure_model(model_path)) {
        return 1;
    }

    // Schema: agent/turns + agent/meta
    std::vector<celer::TableDescriptor> schema{
        {"agent", "turns"},
        {"agent", "meta"},
    };

    // Select factory based on mode
    celer::BackendFactory factory;
    if (mode == "--rocksdb") {
#if CELER_HAS_ROCKSDB
        std::cout << "Backend: RocksDB at " << db_path << "\n";
        factory = celer::backends::rocksdb::factory(
            celer::backends::rocksdb::Config{.path = db_path});
#else
        std::cerr << "Error: compiled without RocksDB. Use --memory or install librocksdb-dev.\n";
        return 1;
#endif
    } else {
        std::cout << "Backend: in-memory (ephemeral)\n";
        factory = make_memory_factory();
    }

    // Build tree + store
    auto root = celer::build_tree(factory, schema);
    if (!root) {
        std::cerr << "Failed to open store: " << root.error().message << "\n";
        return 1;
    }
    celer::Store store{std::move(*root), celer::ResourceStack{}};

    // Run agent
    AgentConfig cfg{.llm_host = host, .llm_port = port};
    Agent agent{std::move(store), cfg};
    agent.run();

    return 0;
}

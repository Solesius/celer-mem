#include "celer/backend/qpdf.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace celer {

#if CELER_HAS_QPDF

// ══════════════════════════════════════════════════════════════════════
// PDF name encoding/decoding — maps arbitrary user keys to valid PDF
// Name objects.  PDF names start with '/' and encode bytes outside the
// safe set as #XX (two hex digits, uppercase).  This is the PDF spec's
// own encoding, so QPDF round-trips it correctly.
// ══════════════════════════════════════════════════════════════════════

namespace {

constexpr auto is_safe_name_char(unsigned char c) noexcept -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

auto encode_pdf_name(std::string_view key) -> std::string {
    std::string result;
    result.reserve(1 + key.size() * 3); // worst case: every byte encoded
    result.push_back('/');
    static constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : key) {
        if (is_safe_name_char(c)) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('#');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 0x0F]);
        }
    }
    return result;
}

auto hex_nibble(char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

auto decode_pdf_name(std::string_view name) -> std::string {
    // Strip leading '/'
    if (!name.empty() && name[0] == '/') name.remove_prefix(1);
    std::string result;
    result.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '#' && i + 2 < name.size()) {
            int hi = hex_nibble(name[i + 1]);
            int lo = hex_nibble(name[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        result.push_back(name[i]);
    }
    return result;
}

/// Validate that a name is safe for use as a scope/table identifier.
/// Same rules as SQLite backend: non-empty, ASCII alphanumeric + underscore.
auto validate_ident(std::string_view name) -> VoidResult {
    if (name.empty()) {
        return std::unexpected(Error{"QPDFValidation", "identifier must not be empty"});
    }
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return std::unexpected(Error{"QPDFValidation",
                "identifier contains invalid character: only [a-zA-Z0-9_] allowed"});
        }
    }
    return {};
}

/// Atomically write PDF to disk: write to temp, then rename.
auto atomic_write_pdf(QPDF& pdf, const std::string& path, bool linearize) -> VoidResult {
    auto tmp_path = path + ".tmp";
    try {
        QPDFWriter writer(pdf, tmp_path.c_str());
        writer.setStaticID(false);
        if (linearize) {
            writer.setLinearization(true);
        }
        writer.write();
    } catch (const std::exception& e) {
        // Clean up temp file on failure
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return std::unexpected(Error{"QPDFWrite",
            "failed to write PDF to '" + path + "': " + e.what()});
    }

    // Atomic rename
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return std::unexpected(Error{"QPDFWrite",
            "failed to rename temp PDF to '" + path + "': " + ec.message()});
    }
    return {};
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
// QPDFScopeState — shared per-scope state
// ══════════════════════════════════════════════════════════════════════

auto QPDFScopeState::flush_locked() -> VoidResult {
    if (!dirty || !pdf) return {};
    auto r = atomic_write_pdf(*pdf, file_path, linearize);
    if (r) dirty = false;
    return r;
}

// ══════════════════════════════════════════════════════════════════════
// QPDFBackend — StorageBackend implementation
// ══════════════════════════════════════════════════════════════════════

auto QPDFBackend::get_table_dict_locked() -> QPDFObjectHandle {
    auto& pdf = *state_->pdf;
    auto root = pdf.getRoot();

    // Get or create /CelerKV dictionary in the catalog
    QPDFObjectHandle celer_kv;
    if (root.hasKey("/CelerKV")) {
        celer_kv = root.getKey("/CelerKV");
    } else {
        celer_kv = QPDFObjectHandle::newDictionary();
        celer_kv = pdf.makeIndirectObject(celer_kv);
        root.replaceKey("/CelerKV", celer_kv);
    }

    // Get or create the table sub-dictionary
    auto table_key = encode_pdf_name(table_name_);
    if (celer_kv.hasKey(table_key)) {
        return celer_kv.getKey(table_key);
    }
    auto table_dict = QPDFObjectHandle::newDictionary();
    table_dict = pdf.makeIndirectObject(table_dict);
    celer_kv.replaceKey(table_key, table_dict);
    state_->dirty = true;
    return table_dict;
}

auto QPDFBackend::get(std::string_view key) -> Result<std::optional<std::string>> {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFGet", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();
    auto pdf_key = encode_pdf_name(key);

    if (!table_dict.hasKey(pdf_key)) {
        return std::optional<std::string>{std::nullopt};
    }

    auto obj = table_dict.getKey(pdf_key);
    if (obj.isString()) {
        return std::optional<std::string>{obj.getStringValue()};
    }
    return std::optional<std::string>{std::nullopt};
}

auto QPDFBackend::put(std::string_view key, std::string_view value) -> VoidResult {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFPut", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();
    auto pdf_key = encode_pdf_name(key);

    table_dict.replaceKey(pdf_key, QPDFObjectHandle::newString(std::string(value)));
    state_->dirty = true;

    // Flush to disk for durability
    return state_->flush_locked();
}

auto QPDFBackend::del(std::string_view key) -> VoidResult {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFDel", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();
    auto pdf_key = encode_pdf_name(key);

    if (table_dict.hasKey(pdf_key)) {
        table_dict.removeKey(pdf_key);
        state_->dirty = true;
        return state_->flush_locked();
    }
    return {};
}

auto QPDFBackend::prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFScan", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();
    auto keys = table_dict.getKeys();

    std::vector<KVPair> results;
    for (const auto& pdf_key : keys) {
        auto decoded = decode_pdf_name(pdf_key);
        if (prefix.empty() || decoded.starts_with(prefix)) {
            auto obj = table_dict.getKey(pdf_key);
            if (obj.isString()) {
                results.push_back(KVPair{std::move(decoded), obj.getStringValue()});
            }
        }
    }

    // Sort by key for consistent ordering (PDF dictionaries are unordered)
    std::sort(results.begin(), results.end(),
              [](const KVPair& a, const KVPair& b) { return a.key < b.key; });

    return results;
}

auto QPDFBackend::batch(std::span<const BatchOp> ops) -> VoidResult {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFBatch", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();

    for (const auto& op : ops) {
        auto pdf_key = encode_pdf_name(op.key);
        if (op.kind == BatchOp::Kind::put) {
            if (!op.value) {
                return std::unexpected(Error{"StoreBatch", "put op missing value"});
            }
            table_dict.replaceKey(pdf_key,
                QPDFObjectHandle::newString(std::string(*op.value)));
            state_->dirty = true;
        } else {
            if (table_dict.hasKey(pdf_key)) {
                table_dict.removeKey(pdf_key);
                state_->dirty = true;
            }
        }
    }

    // Single flush for the whole batch
    return state_->flush_locked();
}

auto QPDFBackend::compact() -> VoidResult {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFCompact", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    // Force a full rewrite (compaction) of the PDF — linearization
    // re-orders objects for streaming and removes unreferenced objects.
    state_->dirty = true;
    return state_->flush_locked();
}

auto QPDFBackend::foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx)
    -> VoidResult {
    if (!state_ || !state_->pdf) {
        return std::unexpected(Error{"QPDFForeach", "backend is closed"});
    }

    std::lock_guard lock(state_->mu);
    auto table_dict = get_table_dict_locked();
    auto keys = table_dict.getKeys();

    // Collect and sort for deterministic order
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& pdf_key : keys) {
        auto decoded = decode_pdf_name(pdf_key);
        if (prefix.empty() || decoded.starts_with(prefix)) {
            auto obj = table_dict.getKey(pdf_key);
            if (obj.isString()) {
                pairs.emplace_back(std::move(decoded), obj.getStringValue());
            }
        }
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [k, v] : pairs) {
        visitor(user_ctx, k, v);
    }
    return {};
}

auto QPDFBackend::close() -> VoidResult {
    if (state_) {
        std::lock_guard lock(state_->mu);
        auto r = state_->flush_locked();
        state_.reset();
        if (!r) return r;
    }
    return {};
}

// ── Streaming stubs (RFC-002) — default implementations via materialization ──

auto QPDFBackend::stream_get(std::string_view key) -> Result<StreamHandle<char>> {
    auto r = get(key);
    if (!r) return std::unexpected(r.error());
    if (!r->has_value()) return stream::empty<char>();
    return stream::from_string(std::move(r->value()));
}

auto QPDFBackend::stream_put(std::string_view key, StreamHandle<char> input) -> VoidResult {
    auto collected = stream::collect_string(input);
    if (!collected) return std::unexpected(collected.error());
    return put(key, *collected);
}

auto QPDFBackend::stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>> {
    auto r = prefix_scan(prefix);
    if (!r) return std::unexpected(r.error());
    return stream::from_vector(std::move(*r));
}

// ══════════════════════════════════════════════════════════════════════
// Factory — open or create PDF files, share QPDF state across tables
// ══════════════════════════════════════════════════════════════════════

namespace {

/// Open an existing PDF or create a new empty one.
auto open_scope_pdf(const backends::qpdf::Config& config, const std::string& pdf_path)
    -> Result<QPDFScopeStatePtr> {
    // Ensure parent directory exists
    auto parent = std::filesystem::path(pdf_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(Error{"QPDFOpen",
                "failed to create directory '" + parent.string() + "': " + ec.message()});
        }
    }

    auto state = std::make_shared<QPDFScopeState>();
    state->file_path = pdf_path;
    state->linearize = config.linearize;
    state->pdf = std::make_unique<QPDF>();

    if (std::filesystem::exists(pdf_path)) {
        try {
            state->pdf->processFile(pdf_path.c_str());
        } catch (const std::exception& e) {
            return std::unexpected(Error{"QPDFOpen",
                "failed to open PDF '" + pdf_path + "': " + e.what()});
        }
    } else if (config.create_if_missing) {
        state->pdf->emptyPDF();
        // Write the initial empty PDF to disk
        state->dirty = true;
        auto r = state->flush_locked();
        if (!r) return std::unexpected(r.error());
    } else {
        return std::unexpected(Error{"QPDFOpen",
            "PDF file '" + pdf_path + "' does not exist and create_if_missing is false"});
    }

    return state;
}

/// Create a QPDFBackend for a specific table within an already-open scope.
auto open_table(const QPDFScopeStatePtr& state, std::string_view table_name)
    -> Result<BackendHandle> {
    if (auto v = validate_ident(table_name); !v) return std::unexpected(v.error());

    auto* backend = new QPDFBackend(state, std::string(table_name));
    return make_backend_handle<QPDFBackend>(backend);
}

} // anonymous namespace

namespace backends::qpdf {

auto factory(Config cfg) -> BackendFactory {
    // Cache: one shared QPDF state per scope.
    struct SharedState {
        std::mutex mu;
        std::unordered_map<std::string, QPDFScopeStatePtr> scope_pdfs;
    };
    auto shared = std::make_shared<SharedState>();

    return [c = std::move(cfg), shared](std::string_view scope, std::string_view table) -> Result<BackendHandle> {
        if (auto v = validate_ident(scope); !v) return std::unexpected(v.error());

        std::string scope_key(scope);
        QPDFScopeStatePtr state;

        {
            std::lock_guard lock(shared->mu);
            auto it = shared->scope_pdfs.find(scope_key);
            if (it != shared->scope_pdfs.end()) {
                state = it->second;
            } else {
                auto pdf_path = c.path + "/" + scope_key + ".pdf";
                auto r = open_scope_pdf(c, pdf_path);
                if (!r) return std::unexpected(r.error());
                state = std::move(*r);
                shared->scope_pdfs[scope_key] = state;
            }
        }

        return open_table(state, table);
    };
}

} // namespace backends::qpdf

#else // CELER_HAS_QPDF == 0

namespace backends::qpdf {

auto factory(Config /*cfg*/) -> BackendFactory {
    return [](std::string_view, std::string_view) -> Result<BackendHandle> {
        return std::unexpected(Error{"NotAvailable", "compiled without QPDF support"});
    };
}

} // namespace backends::qpdf

#endif

} // namespace celer

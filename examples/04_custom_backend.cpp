/// examples/04_custom_backend.cpp — Write your own StorageBackend
/// Demonstrates: satisfying the StorageBackend concept with a pure in-memory backend,
///               then using it through the full Store → DbRef → TableRef API.
///
/// Build: g++ -std=c++23 -I include 04_custom_backend.cpp -L build -lceler -lrocksdb -o custom_backend
/// Run:   ./custom_backend

#include <celer/celer.hpp>
#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>

// ── InMemoryBackend: a trivial backend that stores everything in a std::map ──
// Satisfies celer::StorageBackend concept. No disk I/O, no dependencies.

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
        if (auto it = store_.find(key); it != store_.end()) {
            store_.erase(it);
        }
        return {};
    }

    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> celer::Result<std::vector<celer::KVPair>> {
        std::lock_guard lk(mu_);
        std::vector<celer::KVPair> results;
        auto it = store_.lower_bound(prefix);
        while (it != store_.end() && it->first.starts_with(prefix)) {
            results.push_back(celer::KVPair{it->first, it->second});
            ++it;
        }
        return results;
    }

    [[nodiscard]] auto batch(std::span<const celer::BatchOp> ops) -> celer::VoidResult {
        std::lock_guard lk(mu_);
        for (const auto& op : ops) {
            if (op.kind == celer::BatchOp::Kind::put && op.value) {
                store_[op.key] = *op.value;
            } else {
                store_.erase(op.key);
            }
        }
        return {};
    }

    [[nodiscard]] auto compact() -> celer::VoidResult { return {}; }

    [[nodiscard]] auto foreach_scan(std::string_view prefix, celer::ScanVisitor visitor, void* ctx)
        -> celer::VoidResult {
        std::lock_guard lk(mu_);
        auto it = store_.lower_bound(prefix);
        while (it != store_.end() && it->first.starts_with(prefix)) {
            visitor(ctx, it->first, it->second);
            ++it;
        }
        return {};
    }

private:
    std::map<std::string, std::string, std::less<>> store_;
    std::mutex mu_;
};

// Verify at compile time that InMemoryBackend satisfies the concept
static_assert(celer::StorageBackend<InMemoryBackend>,
    "InMemoryBackend must satisfy celer::StorageBackend");

int main() {
    std::cout << "Custom backend example: InMemoryBackend\n\n";

    // ── 1. Define a BackendFactory from the custom backend ──
    // This is the same shape as celer::backends::rocksdb::factory() returns.
    // Any backend just needs a lambda: (scope, table) -> Result<BackendHandle>.
    celer::BackendFactory mem_factory = [](std::string_view /*scope*/, std::string_view /*table*/)
        -> celer::Result<celer::BackendHandle> {
        return celer::make_backend_handle<InMemoryBackend>(new InMemoryBackend());
    };

    // ── 2. build_tree — one call, backend-agnostic ──
    std::vector<celer::TableDescriptor> schema{{"project", "tasks"}};
    auto root = celer::build_tree(mem_factory, schema);

    // ── 3. Create a Store ──
    celer::Store store{std::move(*root), celer::ResourceStack{}};

    // ── 4. Use the full API exactly like RocksDB ──
    auto tbl = store.db("project")->table("tasks");

    (void)tbl->put_raw("t:1", "Design celer-mem");
    (void)tbl->put_raw("t:2", "Write custom backend");
    (void)tbl->put_raw("t:3", "Ship to prod");

    auto got = tbl->get_raw("t:2");
    std::cout << "  t:2 = " << got->value() << "\n";

    // Prefix scan via table-level API
    auto all = tbl->prefix<std::string>("t:");
    std::cout << "  all tasks (" << all->count() << "):\n";
    all->foreach([](const std::string& val) {
        std::cout << "    → " << val << "\n";
    });

    // Delete
    (void)tbl->del("t:3");
    auto after = tbl->prefix<std::string>("t:");
    std::cout << "  after delete: " << after->count() << " tasks\n";

    // ── 5. Also works with open() for the global singleton ──
    std::cout << "\n  (Also usable with celer::open(mem_factory, schema) — same factory)\n";

    std::cout << "✓ Custom backend works — zero disk I/O, full API\n";
    return 0;
}

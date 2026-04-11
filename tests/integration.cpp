/// tests/integration.cpp — End-to-end integration tests
/// Tests persistence, recovery, concurrent access, error paths, and full lifecycle.
///
/// These complement the unit tests in main.cpp by testing cross-cutting concerns
/// that span multiple layers of the stack.

#include "celer/celer.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static auto base_path() -> fs::path {
    return fs::temp_directory_path() / "celer_integration";
}

static auto fresh(const char* name) -> std::string {
    auto p = base_path() / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}

static auto nuke(const char* name) -> void {
    fs::remove_all(base_path() / name);
}

static int pass_count = 0;
static int fail_count = 0;

#define IT(name) \
    static auto name() -> void; \
    struct name##_reg { name##_reg() { \
        try { name(); ++pass_count; std::cout << "  PASS  " #name "\n"; } \
        catch (const std::exception& e) { ++fail_count; std::cerr << "  FAIL  " #name ": " << e.what() << "\n"; } \
        catch (...) { ++fail_count; std::cerr << "  FAIL  " #name ": unknown\n"; } \
    } } name##_inst; \
    static auto name() -> void

// ════════════════════════════════════════════════════════
// E2E: Persistence — data survives close/reopen
// ════════════════════════════════════════════════════════

IT(should_persist_data_across_close_reopen) {
    const auto* tag = "persist";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    // Write
    {
        auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
        assert(r.has_value());
        auto tbl = celer::db("data")->table("kv");
        (void)tbl->put_raw("greeting", "hello from session 1");
        celer::close();
    }

    // Re-open and verify
    {
        auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
        assert(r.has_value());
        auto tbl = celer::db("data")->table("kv");
        auto got = tbl->get_raw("greeting");
        assert(got.has_value());
        assert(got->has_value());
        assert(got->value() == "hello from session 1");
        celer::close();
    }

    nuke(tag);
}

// ════════════════════════════════════════════════════════
// E2E: Multi-table isolation — tables don't bleed into each other
// ════════════════════════════════════════════════════════

IT(should_isolate_tables_within_scope) {
    const auto* tag = "isolation";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"app", "users"}, {"app", "logs"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(r.has_value());

    auto users = celer::db("app")->table("users");
    auto logs  = celer::db("app")->table("logs");

    (void)users->put_raw("u-1", "alice");
    (void)logs->put_raw("l-1", "started");

    // users table shouldn't see log keys
    auto u_miss = users->get_raw("l-1");
    assert(u_miss.has_value() && !u_miss->has_value());

    // logs table shouldn't see user keys
    auto l_miss = logs->get_raw("u-1");
    assert(l_miss.has_value() && !l_miss->has_value());

    celer::close();
    nuke(tag);
}

// ════════════════════════════════════════════════════════
// E2E: Large dataset — write + scan 10K records
// ════════════════════════════════════════════════════════

IT(should_handle_10k_records) {
    const auto* tag = "large_dataset";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"bench", "data"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(r.has_value());

    auto tbl = celer::db("bench")->table("data");

    // Write 10K records
    for (int i = 0; i < 10'000; ++i) {
        auto key = "rec-" + std::to_string(i);
        auto val = "value-" + std::to_string(i);
        auto pr  = tbl->put_raw(key, val);
        assert(pr.has_value());
    }

    // Verify count via table-level typed scan
    auto all = tbl->all<std::string>();
    assert(all.has_value());
    assert(all->count() == 10'000);

    // Spot-check
    auto r5000 = tbl->get_raw("rec-5000");
    assert(r5000.has_value() && r5000->has_value());
    assert(r5000->value() == "value-5000");

    celer::close();
    nuke(tag);
}

// ════════════════════════════════════════════════════════
// E2E: Manual Store construction (no global API)
// ════════════════════════════════════════════════════════

IT(should_work_with_instance_api) {
    const auto* tag = "instance_api";
    auto dir = fresh(tag);

    // build_tree makes manual Store construction backend-agnostic
    celer::backends::rocksdb::Config cfg{.path = dir};
    std::vector<celer::TableDescriptor> schema{
        {"project", "tasks"},
        {"project", "notes"},
    };

    auto root = celer::build_tree(celer::backends::rocksdb::factory(cfg), schema);
    assert(root.has_value());

    celer::Store store{std::move(*root), celer::ResourceStack{}};

    // Use it
    auto tbl_t = store.db("project")->table("tasks");
    auto tbl_n = store.db("project")->table("notes");

    (void)tbl_t->put_raw("t-1", "do stuff");
    (void)tbl_n->put_raw("n-1", "remember things");

    auto got_t = tbl_t->get_raw("t-1");
    assert(got_t.has_value() && got_t->has_value() && got_t->value() == "do stuff");
    auto got_n = tbl_n->get_raw("n-1");
    assert(got_n.has_value() && got_n->has_value() && got_n->value() == "remember things");

    store.destroy();
    nuke(tag);
}

// ════════════════════════════════════════════════════════
// E2E: Typed put/get via Codec<std::string>
// ════════════════════════════════════════════════════════

IT(should_round_trip_typed_strings) {
    const auto* tag = "typed_strings";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(r.has_value());

    auto tbl = celer::db("data")->table("kv");

    // Use typed API with std::string (identity codec)
    auto pr = tbl->put<std::string>("typed-key", std::string("typed-value"));
    assert(pr.has_value());

    auto got = tbl->get<std::string>("typed-key");
    assert(got.has_value());
    assert(got->has_value());
    assert(got->value() == "typed-value");

    // Miss path
    auto miss = tbl->get<std::string>("nonexistent");
    assert(miss.has_value());
    assert(!miss->has_value());

    celer::close();
    nuke(tag);
}

// ════════════════════════════════════════════════════════
// E2E: Error paths — double open, bad scope, bad table
// ════════════════════════════════════════════════════════

IT(should_reject_double_open) {
    const auto* tag = "double_open";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    auto r1 = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(r1.has_value());

    auto r2 = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(!r2.has_value());
    assert(r2.error().code == "AlreadyOpen");

    celer::close();
    nuke(tag);
}

IT(should_error_on_bad_scope) {
    const auto* tag = "bad_scope";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    (void)celer::open(celer::backends::rocksdb::factory(cfg), schema);
    auto bad = celer::db("nope");
    assert(!bad.has_value());
    assert(bad.error().code == "ScopeNotFound");

    celer::close();
    nuke(tag);
}

IT(should_error_on_bad_table) {
    const auto* tag = "bad_table";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    (void)celer::open(celer::backends::rocksdb::factory(cfg), schema);
    auto db = celer::db("data");
    assert(db.has_value());
    auto bad = db->table("nonexistent");
    assert(!bad.has_value());
    assert(bad.error().code == "TableNotFound");

    celer::close();
    nuke(tag);
}

IT(should_error_when_not_initialized) {
    auto r = celer::db("anything");
    assert(!r.has_value());
    assert(r.error().code == "NotInitialized");

    // close on uninitialized is fine
    auto c = celer::close();
    assert(c.has_value());
}

// ════════════════════════════════════════════════════════
// E2E: Concurrent reads (RocksDB is thread-safe per RFC §9)
// ════════════════════════════════════════════════════════

IT(should_handle_concurrent_reads) {
    const auto* tag = "concurrent";
    auto dir = fresh(tag);
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::rocksdb::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    assert(r.has_value());

    auto tbl = celer::db("data")->table("kv");

    // Seed data
    for (int i = 0; i < 100; ++i) {
        (void)tbl->put_raw("k-" + std::to_string(i), "v-" + std::to_string(i));
    }

    // Concurrent reads from 4 threads
    std::vector<std::thread> threads;
    std::atomic<int> total_reads{0};

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&total_reads]() {
            auto db = celer::db("data");
            if (!db) return;
            auto tbl = db->table("kv");
            if (!tbl) return;
            for (int i = 0; i < 100; ++i) {
                auto got = tbl->get_raw("k-" + std::to_string(i));
                if (got && got->has_value()) {
                    ++total_reads;
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    assert(total_reads == 400);  // 4 threads × 100 reads

    celer::close();
    nuke(tag);
}

// ── Entry point ──

int main() {
    std::cout << "celer-mem integration test suite\n";
    std::cout << "════════════════════════════════════\n";
    // Tests auto-register via static construction.
    std::cout << "════════════════════════════════════\n";
    std::cout << pass_count << " passed, " << fail_count << " failed\n";
    return fail_count > 0 ? 1 : 0;
}

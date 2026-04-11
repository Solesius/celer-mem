/// tests/test_sqlite.cpp — SQLite backend tests
/// Tests the full stack: Store → DbRef → TableRef → BackendHandle → SQLite

#include "celer/celer.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ── Helpers ──

static auto tmp_dir(const char* name) -> std::string {
    auto p = fs::temp_directory_path() / "celer_sqlite_test" / name;
    fs::create_directories(p);
    return p.string();
}

static auto cleanup(const char* name) -> void {
    auto p = fs::temp_directory_path() / "celer_sqlite_test" / name;
    fs::remove_all(p);
}

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
    static auto name() -> void; \
    struct name##_reg { name##_reg() { \
        try { name(); ++pass_count; std::cout << "  PASS  " #name "\n"; } \
        catch (const std::exception& e) { ++fail_count; std::cerr << "  FAIL  " #name ": " << e.what() << "\n"; } \
        catch (...) { ++fail_count; std::cerr << "  FAIL  " #name ": unknown exception\n"; } \
    } } name##_inst; \
    static auto name() -> void

#define ASSERT_OK(expr) do { auto _r_ = (expr); assert(_r_.has_value()); } while(0)
#define ASSERT_ERR(expr, ecode) do { auto _r_ = (expr); assert(!_r_.has_value()); assert(_r_.error().code == (ecode)); } while(0)

// ════════════════════════════════════════
// SQLite backend — raw BackendHandle tests
// ════════════════════════════════════════

TEST(test_sqlite_create_backend) {
    cleanup("create_backend");
    auto dir = tmp_dir("create_backend");
    celer::backends::sqlite::Config cfg{.path = dir, .create_if_missing = true};
    auto r = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(r.has_value());
    cleanup("create_backend");
}

TEST(test_sqlite_put_get_del) {
    cleanup("put_get_del");
    auto dir = tmp_dir("put_get_del");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    // put
    ASSERT_OK(handle->put("key1", "value1"));
    ASSERT_OK(handle->put("key2", "value2"));

    // get — hit
    auto r1 = handle->get("key1");
    assert(r1.has_value());
    assert(r1->has_value());
    assert(r1->value() == "value1");

    // get — miss
    auto r2 = handle->get("nonexistent");
    assert(r2.has_value());
    assert(!r2->has_value());

    // del
    ASSERT_OK(handle->del("key1"));
    auto r3 = handle->get("key1");
    assert(r3.has_value());
    assert(!r3->has_value());

    cleanup("put_get_del");
}

TEST(test_sqlite_overwrite) {
    cleanup("overwrite");
    auto dir = tmp_dir("overwrite");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    ASSERT_OK(handle->put("k", "v1"));
    ASSERT_OK(handle->put("k", "v2"));
    auto r = handle->get("k");
    assert(r.has_value() && r->has_value() && r->value() == "v2");

    cleanup("overwrite");
}

TEST(test_sqlite_prefix_scan) {
    cleanup("prefix_scan");
    auto dir = tmp_dir("prefix_scan");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    ASSERT_OK(handle->put("task:001", "a"));
    ASSERT_OK(handle->put("task:002", "b"));
    ASSERT_OK(handle->put("task:003", "c"));
    ASSERT_OK(handle->put("note:001", "d"));

    auto scan = handle->prefix_scan("task:");
    assert(scan.has_value());
    assert(scan->size() == 3);

    // Empty prefix → all keys
    auto all = handle->prefix_scan("");
    assert(all.has_value());
    assert(all->size() == 4);

    cleanup("prefix_scan");
}

TEST(test_sqlite_batch) {
    cleanup("batch");
    auto dir = tmp_dir("batch");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    std::vector<celer::BatchOp> ops{
        {celer::BatchOp::Kind::put, "", "k1", "v1"},
        {celer::BatchOp::Kind::put, "", "k2", "v2"},
        {celer::BatchOp::Kind::put, "", "k3", "v3"},
    };
    ASSERT_OK(handle->batch(ops));

    auto r1 = handle->get("k1");
    assert(r1.has_value() && r1->has_value() && r1->value() == "v1");
    auto r2 = handle->get("k2");
    assert(r2.has_value() && r2->has_value() && r2->value() == "v2");

    // Batch delete
    std::vector<celer::BatchOp> del_ops{
        {celer::BatchOp::Kind::del, "", "k1", std::nullopt},
    };
    ASSERT_OK(handle->batch(del_ops));
    auto r3 = handle->get("k1");
    assert(r3.has_value() && !r3->has_value());

    cleanup("batch");
}

TEST(test_sqlite_foreach_scan) {
    cleanup("foreach");
    auto dir = tmp_dir("foreach");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    ASSERT_OK(handle->put("a:1", "x"));
    ASSERT_OK(handle->put("a:2", "y"));
    ASSERT_OK(handle->put("b:1", "z"));

    int count = 0;
    struct Ctx { int* count; };
    Ctx ctx{&count};
    auto r = handle->foreach_scan("a:",
        [](void* raw, std::string_view /*k*/, std::string_view /*v*/) {
            auto* c = static_cast<Ctx*>(raw);
            ++(*c->count);
        }, &ctx);
    ASSERT_OK(r);
    assert(count == 2);

    cleanup("foreach");
}

TEST(test_sqlite_compact) {
    cleanup("compact");
    auto dir = tmp_dir("compact");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());
    ASSERT_OK(handle->put("x", "y"));
    ASSERT_OK(handle->compact());
    cleanup("compact");
}

// ════════════════════════════════════════
// Composite tree + dispatch tests (SQLite)
// ════════════════════════════════════════

TEST(test_sqlite_tree_dispatch_put_get) {
    cleanup("tree_dispatch");
    auto dir = tmp_dir("tree_dispatch");
    celer::backends::sqlite::Config cfg{.path = dir};
    auto handle = celer::backends::sqlite::factory(cfg)("_", "_");
    assert(handle.has_value());

    auto leaf = celer::build_leaf("tasks", std::move(*handle));
    assert(leaf.has_value());

    ASSERT_OK(celer::node_put(*leaf, "001", "hello"));
    auto r = celer::node_get(*leaf, "001");
    assert(r.has_value());
    assert(r->has_value());
    assert(r->value() == "hello");

    cleanup("tree_dispatch");
}

// ════════════════════════════════════════
// Store → DbRef → TableRef (full API with SQLite)
// ════════════════════════════════════════

TEST(test_sqlite_build_tree) {
    cleanup("build_tree");
    auto base = tmp_dir("build_tree");

    celer::backends::sqlite::Config cfg{.path = base};
    std::vector<celer::TableDescriptor> schema{
        {"project", "tasks"},
        {"project", "notes"},
    };

    auto root = celer::build_tree(celer::backends::sqlite::factory(cfg), schema);
    assert(root.has_value());

    celer::Store store{std::move(*root), celer::ResourceStack{}};

    auto db = store.db("project");
    assert(db.has_value());
    auto tasks = db->table("tasks");
    auto notes = db->table("notes");
    assert(tasks.has_value() && notes.has_value());

    // Write + read isolates across tables
    ASSERT_OK(tasks->put_raw("t1", "do stuff"));
    ASSERT_OK(notes->put_raw("n1", "remember stuff"));
    auto t = tasks->get_raw("t1");
    auto n = notes->get_raw("n1");
    assert(t.has_value() && t->has_value() && t->value() == "do stuff");
    assert(n.has_value() && n->has_value() && n->value() == "remember stuff");

    // Cross-table isolation
    auto miss = tasks->get_raw("n1");
    assert(miss.has_value() && !miss->has_value());

    cleanup("build_tree");
}

// ════════════════════════════════════════
// Global API with SQLite
// ════════════════════════════════════════

TEST(test_sqlite_global_api) {
    cleanup("global_api");
    auto dir = tmp_dir("global_api");

    std::vector<celer::TableDescriptor> tables{
        {"project", "tasks"},
        {"project", "notes"},
    };

    celer::backends::sqlite::Config cfg{.path = dir};
    ASSERT_OK(celer::open(celer::backends::sqlite::factory(cfg), tables));

    auto db = celer::db("project");
    assert(db.has_value());
    auto tbl = db->table("tasks");
    assert(tbl.has_value());

    ASSERT_OK(tbl->put_raw("hello", "world"));
    auto got = tbl->get_raw("hello");
    assert(got.has_value() && got->has_value() && got->value() == "world");

    ASSERT_OK(celer::close());
    ASSERT_OK(celer::close()); // double close is fine
    ASSERT_ERR(celer::db("project"), "NotInitialized");

    cleanup("global_api");
}

// ════════════════════════════════════════
// Persistence — data survives close/reopen
// ════════════════════════════════════════

TEST(test_sqlite_persistence) {
    cleanup("persist");
    auto dir = tmp_dir("persist");
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::sqlite::Config cfg{.path = dir};

    // Write
    {
        auto r = celer::open(celer::backends::sqlite::factory(cfg), schema);
        assert(r.has_value());
        auto tbl = celer::db("data")->table("kv");
        (void)tbl->put_raw("greeting", "hello from session 1");
        celer::close();
    }

    // Re-open and verify
    {
        auto r = celer::open(celer::backends::sqlite::factory(cfg), schema);
        assert(r.has_value());
        auto tbl = celer::db("data")->table("kv");
        auto got = tbl->get_raw("greeting");
        assert(got.has_value());
        assert(got->has_value());
        assert(got->value() == "hello from session 1");
        celer::close();
    }

    cleanup("persist");
}

// ════════════════════════════════════════
// Large dataset — 10K records
// ════════════════════════════════════════

TEST(test_sqlite_10k_records) {
    cleanup("large_dataset");
    auto dir = tmp_dir("large_dataset");
    std::vector<celer::TableDescriptor> schema{{"bench", "data"}};
    celer::backends::sqlite::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::sqlite::factory(cfg), schema);
    assert(r.has_value());

    auto tbl = celer::db("bench")->table("data");

    for (int i = 0; i < 10'000; ++i) {
        auto key = "rec-" + std::to_string(i);
        auto val = "value-" + std::to_string(i);
        auto pr  = tbl->put_raw(key, val);
        assert(pr.has_value());
    }

    auto all = tbl->all<std::string>();
    assert(all.has_value());
    assert(all->count() == 10'000);

    auto r5000 = tbl->get_raw("rec-5000");
    assert(r5000.has_value() && r5000->has_value());
    assert(r5000->value() == "value-5000");

    celer::close();
    cleanup("large_dataset");
}

// ════════════════════════════════════════
// Concurrent reads (SQLite with WAL)
// ════════════════════════════════════════

TEST(test_sqlite_concurrent_reads) {
    cleanup("concurrent");
    auto dir = tmp_dir("concurrent");
    std::vector<celer::TableDescriptor> schema{{"data", "kv"}};
    celer::backends::sqlite::Config cfg{.path = dir};

    auto r = celer::open(celer::backends::sqlite::factory(cfg), schema);
    assert(r.has_value());

    auto tbl = celer::db("data")->table("kv");

    for (int i = 0; i < 100; ++i) {
        (void)tbl->put_raw("k-" + std::to_string(i), "v-" + std::to_string(i));
    }

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
    assert(total_reads == 400);

    celer::close();
    cleanup("concurrent");
}

// ── Entry point ──

int main() {
    std::cout << "celer-mem SQLite backend test suite\n";
    std::cout << "───────────────────────────────────\n";
    // Tests auto-register via static construction above.
    std::cout << "───────────────────────────────────\n";
    std::cout << pass_count << " passed, " << fail_count << " failed\n";
    return fail_count > 0 ? 1 : 0;
}

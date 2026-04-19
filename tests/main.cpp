/// celer-mem integration tests — RocksDB backend
/// Tests the full stack: Store → DbRef → TableRef → BackendHandle → RocksDB

#include "celer/celer.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "test_support.hpp"

namespace fs = std::filesystem;

// ── Helpers ──

static auto tmp_dir(const char* name) -> std::string {
    auto p = fs::temp_directory_path() / "celer_test" / name;
    fs::create_directories(p);
    return p.string();
}

static auto cleanup(const char* name) -> void {
    auto p = fs::temp_directory_path() / "celer_test" / name;
    fs::remove_all(p);
}

#undef TEST
#define TEST(name) GTEST_TEST(MainSuite, name)

#define ASSERT_OK(expr) do { auto _r_ = (expr); ASSERT_TRUE(_r_.has_value()) << _r_.error().code << ": " << _r_.error().message; } while(0)
#define ASSERT_ERR(expr, ecode) do { auto _r_ = (expr); ASSERT_FALSE(_r_.has_value()); ASSERT_EQ(_r_.error().code, (ecode)); } while(0)

// ════════════════════════════════════════
// Unit tests — pure types, no RocksDB I/O
// ════════════════════════════════════════

TEST(test_result_ok) {
    auto r = celer::Result<int>{42};
    assert(celer::is_ok(r));
    assert(*r == 42);
}

TEST(test_result_err) {
    auto r = celer::Result<int>{std::unexpected(celer::Error{"TestErr", "oops"})};
    assert(!celer::is_ok(r));
    assert(r.error().code == "TestErr");
}

TEST(test_void_result) {
    celer::VoidResult ok{};
    assert(celer::is_ok(ok));
    celer::VoidResult err{std::unexpected(celer::Error{"E", "msg"})};
    assert(!celer::is_ok(err));
}

TEST(test_resource_stack_lifecycle) {
    int counter = 0;
    {
        celer::ResourceStack stack;
        stack.push("a", &counter, [](void* p) { ++(*static_cast<int*>(p)); });
        stack.push("b", &counter, [](void* p) { ++(*static_cast<int*>(p)); });
        assert(stack.size() == 2);
    }
    assert(counter == 2);
}

TEST(test_fixed_string) {
    constexpr celer::fixed_string fs{"hello"};
    static_assert(fs.size == 5);
    assert(fs.view() == "hello");
}

TEST(test_schema_compile_time) {
    struct Task { std::string id; std::string title; };
    using MySchema = celer::Schema<celer::Bind<"project", "tasks", Task>>;
    static_assert(MySchema::binding_count == 1);
    static_assert(MySchema::has_binding<"project", "tasks">);
    static_assert(!MySchema::has_binding<"project", "nope">);
}

TEST(test_result_set_basics) {
    std::vector<int> data{3, 1, 4, 1, 5, 9};
    celer::ResultSet<int> rs{data};
    assert(rs.count() == 6);
    assert(rs.first().value() == 3);
    auto filtered = rs.filter([](int x) { return x > 3; });
    assert(filtered.count() == 3);
    auto sorted = filtered.sort_by([](int a, int b) { return a < b; });
    assert(sorted.first().value() == 4);
    auto taken = sorted.take(2);
    assert(taken.count() == 2);
}

// ════════════════════════════════════════
// RocksDB backend — raw BackendHandle tests
// ════════════════════════════════════════

class RocksDbBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup("availability_probe");
        auto probe = celer::backends::rocksdb::factory({.path = tmp_dir("availability_probe")})(
            kProbeScope, kProbeTable);
        ASSERT_HAS_VALUE_OR_SKIP_NOT_AVAILABLE(probe);
        cleanup("availability_probe");
    }
};

#undef TEST
#define TEST(name) TEST_F(RocksDbBackendTest, name)

TEST(test_rocksdb_create_backend) {
    cleanup("create_backend");
    auto dir = tmp_dir("create_backend");
    celer::backends::rocksdb::Config cfg{.path = dir, .create_if_missing = true};
    auto r = celer::backends::rocksdb::factory(cfg)("_", "_");
    assert(r.has_value());
    cleanup("create_backend");
}

TEST(test_rocksdb_put_get_del) {
    cleanup("put_get_del");
    auto dir = tmp_dir("put_get_del");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
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

TEST(test_rocksdb_prefix_scan) {
    cleanup("prefix_scan");
    auto dir = tmp_dir("prefix_scan");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
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

TEST(test_rocksdb_batch) {
    cleanup("batch");
    auto dir = tmp_dir("batch");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
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

TEST(test_rocksdb_foreach_scan) {
    cleanup("foreach");
    auto dir = tmp_dir("foreach");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
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

TEST(test_rocksdb_compact) {
    cleanup("compact");
    auto dir = tmp_dir("compact");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
    assert(handle.has_value());
    ASSERT_OK(handle->put("x", "y"));
    ASSERT_OK(handle->compact());
    cleanup("compact");
}

// ════════════════════════════════════════
// Composite tree + dispatch tests
// ════════════════════════════════════════

TEST(test_tree_dispatch_put_get) {
    cleanup("tree_dispatch");
    auto dir = tmp_dir("tree_dispatch");
    celer::backends::rocksdb::Config cfg{.path = dir};
    auto handle = celer::backends::rocksdb::factory(cfg)("_", "_");
    assert(handle.has_value());

    auto leaf = celer::build_leaf("tasks", std::move(*handle));
    assert(leaf.has_value());

    // Direct leaf dispatch
    ASSERT_OK(celer::node_put(*leaf, "001", "hello"));
    auto r = celer::node_get(*leaf, "001");
    assert(r.has_value());
    assert(r->has_value());
    assert(r->value() == "hello");

    cleanup("tree_dispatch");
}

TEST(test_composite_routing) {
    cleanup("routing");
    auto base = tmp_dir("routing");

    // Two leaves under one composite
    celer::backends::rocksdb::Config cfg1{.path = base + "/tasks"};
    celer::backends::rocksdb::Config cfg2{.path = base + "/notes"};
    auto h1 = celer::backends::rocksdb::factory(cfg1)("_", "_");
    auto h2 = celer::backends::rocksdb::factory(cfg2)("_", "_");
    assert(h1.has_value() && h2.has_value());

    auto l1 = celer::build_leaf("tasks", std::move(*h1));
    auto l2 = celer::build_leaf("notes", std::move(*h2));
    assert(l1.has_value() && l2.has_value());

    std::vector<celer::StoreNode> children;
    children.push_back(std::move(*l1));
    children.push_back(std::move(*l2));
    auto comp = celer::build_composite("project", std::move(children));
    assert(comp.has_value());

    // Route through composite: "tasks:key1" → tasks leaf → key "key1"
    ASSERT_OK(celer::node_put(*comp, "tasks:mykey", "myval"));
    auto r = celer::node_get(*comp, "tasks:mykey");
    assert(r.has_value() && r->has_value() && r->value() == "myval");

    // Route to notes
    ASSERT_OK(celer::node_put(*comp, "notes:n1", "note_val"));
    auto r2 = celer::node_get(*comp, "notes:n1");
    assert(r2.has_value() && r2->has_value() && r2->value() == "note_val");

    // Routing to nonexistent child
    auto r3 = celer::node_get(*comp, "doesntexist:k");
    assert(!r3.has_value());
    assert(r3.error().code == "ChildNotFound");

    cleanup("routing");
}

// ════════════════════════════════════════
// Store → DbRef → TableRef (full API stack)
// ════════════════════════════════════════

TEST(test_build_tree) {
    cleanup("build_tree");
    auto base = tmp_dir("build_tree");

    // build_tree is backend-agnostic — same call for SQLite, RocksDB, anything
    celer::backends::rocksdb::Config cfg{.path = base};
    std::vector<celer::TableDescriptor> schema{
        {"project", "tasks"},
        {"project", "notes"},
    };

    auto root = celer::build_tree(celer::backends::rocksdb::factory(cfg), schema);
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

TEST(test_store_full_api) {
    cleanup("store_api");
    auto base = tmp_dir("store_api");

    // Build tree: root → project → {tasks, notes}
    celer::backends::rocksdb::Config cfg_t{.path = base + "/tasks"};
    celer::backends::rocksdb::Config cfg_n{.path = base + "/notes"};
    auto h_t = celer::backends::rocksdb::factory(cfg_t)("_", "_");
    auto h_n = celer::backends::rocksdb::factory(cfg_n)("_", "_");
    assert(h_t.has_value() && h_n.has_value());

    auto l_t = celer::build_leaf("tasks", std::move(*h_t));
    auto l_n = celer::build_leaf("notes", std::move(*h_n));
    assert(l_t.has_value() && l_n.has_value());

    std::vector<celer::StoreNode> leaves;
    leaves.push_back(std::move(*l_t));
    leaves.push_back(std::move(*l_n));
    auto project = celer::build_composite("project", std::move(leaves));
    assert(project.has_value());

    std::vector<celer::StoreNode> scopes;
    scopes.push_back(std::move(*project));
    auto root = celer::build_composite("root", std::move(scopes));
    assert(root.has_value());

    celer::Store store{std::move(*root), celer::ResourceStack{}};

    // db("project") → DbRef
    auto db = store.db("project");
    assert(db.has_value());

    // table("tasks") → TableRef
    auto tbl = db->table("tasks");
    assert(tbl.has_value());

    // Raw string put/get through the handle
    ASSERT_OK(tbl->put<std::string>("t:1", std::string("write tests")));
    auto got = tbl->get<std::string>("t:1");

    // Scope not found
    auto bad = store.db("nonexistent");
    assert(!bad.has_value());
    assert(bad.error().code == "ScopeNotFound");

    // Table not found
    auto bad_tbl = db->table("nonexistent");
    assert(!bad_tbl.has_value());
    assert(bad_tbl.error().code == "TableNotFound");

    cleanup("store_api");
}

// ════════════════════════════════════════
// Global API (celer::open / db / close)
// ════════════════════════════════════════

TEST(test_global_api) {
    cleanup("global_api");
    auto dir = tmp_dir("global_api");

    std::vector<celer::TableDescriptor> tables{
        {"project", "tasks"},
        {"project", "notes"},
    };

    celer::backends::rocksdb::Config cfg{.path = dir};
    ASSERT_OK(celer::open(celer::backends::rocksdb::factory(cfg), tables));

    // Navigate
    auto db = celer::db("project");
    assert(db.has_value());
    auto tbl = db->table("tasks");
    assert(tbl.has_value());

    // Put/get at handle level
    ASSERT_OK(tbl->put_raw("hello", "world"));
    auto got = tbl->get_raw("hello");
    assert(got.has_value() && got->has_value() && got->value() == "world");

    // Close
    ASSERT_OK(celer::close());

    // Double close is fine
    ASSERT_OK(celer::close());

    // After close, db fails
    ASSERT_ERR(celer::db("project"), "NotInitialized");

    cleanup("global_api");
}

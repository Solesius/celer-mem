/// celer-mem test scaffold
/// Minimal: just verifies the library compiles and links.

#include "celer/celer.hpp"

#include <cassert>
#include <iostream>
#include <string>

static auto test_result_ok() -> void {
    auto r = celer::Result<int>{42};
    assert(celer::is_ok(r));
    assert(*r == 42);
}

static auto test_result_err() -> void {
    auto r = celer::Result<int>{std::unexpected(celer::Error{"TestErr", "oops"})};
    assert(!celer::is_ok(r));
    assert(r.error().code == "TestErr");
}

static auto test_void_result() -> void {
    celer::VoidResult ok{};
    assert(celer::is_ok(ok));

    celer::VoidResult err{std::unexpected(celer::Error{"E", "msg"})};
    assert(!celer::is_ok(err));
}

static auto test_resource_stack_lifecycle() -> void {
    int counter = 0;
    {
        celer::ResourceStack stack;
        stack.push("a", &counter, [](void* p) { ++(*static_cast<int*>(p)); });
        stack.push("b", &counter, [](void* p) { ++(*static_cast<int*>(p)); });
        assert(stack.size() == 2);
    }
    // Destructor should have called both destroy fns
    assert(counter == 2);
}

static auto test_composite_construction() -> void {
    auto leaf_r = celer::build_leaf("tasks", nullptr);
    assert(leaf_r.has_value());

    std::vector<celer::StoreNode> children;
    children.push_back(std::move(*leaf_r));

    auto comp_r = celer::build_composite("project", std::move(children));
    assert(comp_r.has_value());

    assert(celer::node_name(*comp_r) == "project");
}

static auto test_store_scope_not_found() -> void {
    auto leaf_r = celer::build_leaf("tasks", nullptr);
    assert(leaf_r.has_value());

    std::vector<celer::StoreNode> children;
    children.push_back(std::move(*leaf_r));

    auto root_r = celer::build_composite("root", std::move(children));
    assert(root_r.has_value());

    celer::Store store{std::move(*root_r), celer::ResourceStack{}};

    auto ok  = store.db("tasks");
    assert(ok.has_value());

    auto err = store.db("nonexistent");
    assert(!err.has_value());
    assert(err.error().code == "ScopeNotFound");
}

static auto test_fixed_string() -> void {
    constexpr celer::fixed_string fs{"hello"};
    static_assert(fs.size == 5);
    assert(fs.view() == "hello");
}

static auto test_schema_compile_time() -> void {
    struct Task { std::string id; std::string title; };

    using MySchema = celer::Schema<
        celer::Bind<"project", "tasks", Task>
    >;

    static_assert(MySchema::binding_count == 1);
    static_assert(MySchema::has_binding<"project", "tasks">);
    static_assert(!MySchema::has_binding<"project", "nope">);
}

static auto test_global_not_initialized() -> void {
    auto r = celer::db("anything");
    assert(!r.has_value());
    assert(r.error().code == "NotInitialized");

    // close() should be idempotent
    auto c1 = celer::close();
    assert(celer::is_ok(c1));
    auto c2 = celer::close();
    assert(celer::is_ok(c2));
}

static auto test_dispatch_stubs_return_not_implemented() -> void {
    auto leaf = celer::StoreNode{celer::ColumnLeaf{"test", nullptr}};

    auto r1 = celer::node_get(leaf, "key");
    assert(!r1.has_value());
    assert(r1.error().code == "NotImplemented");

    auto r2 = celer::node_put(leaf, "key", "val");
    assert(!r2.has_value());

    auto r3 = celer::node_del(leaf, "key");
    assert(!r3.has_value());

    auto r4 = celer::node_prefix_scan(leaf, "");
    assert(!r4.has_value());

    auto r5 = celer::node_compact(leaf);
    assert(!r5.has_value());
}

static auto test_result_set_basics() -> void {
    std::vector<int> data{3, 1, 4, 1, 5, 9};
    celer::ResultSet<int> rs{data};

    assert(rs.count() == 6);
    assert(rs.first().value() == 3);

    auto filtered = rs.filter([](int x) { return x > 3; });
    assert(filtered.count() == 3); // 4, 5, 9

    auto sorted = filtered.sort_by([](int a, int b) { return a < b; });
    assert(sorted.first().value() == 4);

    auto taken = sorted.take(2);
    assert(taken.count() == 2);
}

int main() {
    test_result_ok();
    test_result_err();
    test_void_result();
    test_resource_stack_lifecycle();
    test_composite_construction();
    test_store_scope_not_found();
    test_fixed_string();
    test_schema_compile_time();
    test_global_not_initialized();
    test_dispatch_stubs_return_not_implemented();
    test_result_set_basics();

    std::cout << "✓ All " << 11 << " tests passed\n";
    return 0;
}

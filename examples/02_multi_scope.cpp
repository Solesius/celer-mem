/// examples/02_multi_scope.cpp — Multi-scope hierarchical store
/// Demonstrates: multiple scopes, multiple tables, cross-scope navigation
///
/// Tree layout:
///   root
///   ├── project
///   │   ├── tasks
///   │   └── notes
///   └── analytics
///       └── spans
///
/// Build: g++ -std=c++23 -I include 02_multi_scope.cpp -L build -lceler -lrocksdb -o multi_scope
/// Run:   ./multi_scope

#include <celer/celer.hpp>
#include <iostream>
#include <filesystem>

int main() {
    namespace fs = std::filesystem;
    const auto db_path = fs::temp_directory_path() / "celer_multi_scope";
    fs::remove_all(db_path);

    // ── Define a multi-scope schema ──
    std::vector<celer::TableDescriptor> schema{
        {"project",   "tasks"},
        {"project",   "notes"},
        {"analytics", "spans"},
    };

    celer::StoreConfig cfg{.path = db_path.string()};
    auto r = celer::open(cfg, schema);
    if (!r) { std::cerr << "open: " << r.error().message << "\n"; return 1; }
    std::cout << "✓ Store opened with 2 scopes, 3 tables\n";

    // ── Write to project/tasks ──
    auto tasks = celer::db("project")->table("tasks");
    (void)tasks->put_raw("t:1", "Design RFC");
    (void)tasks->put_raw("t:2", "Implement backend");

    // ── Write to project/notes ──
    auto notes = celer::db("project")->table("notes");
    (void)notes->put_raw("n:1", "Remember to add error handling");

    // ── Write to analytics/spans ──
    auto spans = celer::db("analytics")->table("spans");
    (void)spans->put_raw("span:001", R"({"name":"db.open","dur_ms":12})");
    (void)spans->put_raw("span:002", R"({"name":"db.put","dur_ms":1})");

    // ── Read back from each ──
    auto t1 = tasks->get_raw("t:1");
    std::cout << "  project/tasks/t:1 = " << t1->value() << "\n";

    auto n1 = notes->get_raw("n:1");
    std::cout << "  project/notes/n:1 = " << n1->value() << "\n";

    auto s1 = spans->get_raw("span:001");
    std::cout << "  analytics/spans/span:001 = " << s1->value() << "\n";

    // ── Cross-scope isolation: analytics can't see project data ──
    auto miss = spans->get_raw("t:1");
    if (miss && !miss->has_value()) {
        std::cout << "✓ Cross-scope isolation confirmed — analytics/spans can't see project keys\n";
    }

    // ── Navigate to a scope that doesn't exist ──
    auto bad = celer::db("nonexistent");
    if (!bad) {
        std::cout << "✓ ScopeNotFound: " << bad.error().message << "\n";
    }

    celer::close();
    std::cout << "✓ Closed\n";

    fs::remove_all(db_path);
    return 0;
}

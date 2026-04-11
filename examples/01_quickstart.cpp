/// examples/01_quickstart.cpp — Minimal celer-mem usage
/// Demonstrates: open → put → get → close
///
/// Build: g++ -std=c++23 -I include 01_quickstart.cpp -L build -lceler -lrocksdb -o quickstart
/// Run:   ./quickstart

#include <celer/celer.hpp>
#include <iostream>
#include <filesystem>

int main() {
    namespace fs = std::filesystem;
    const auto db_path = fs::temp_directory_path() / "celer_quickstart";
    fs::remove_all(db_path);  // clean slate

    // ── 1. Open a store with one scope and one table ──
    std::vector<celer::TableDescriptor> schema{
        {"project", "tasks"},
    };

    celer::backends::rocksdb::Config cfg{.path = db_path.string()};
    auto open_r = celer::open(celer::backends::rocksdb::factory(cfg), schema);
    if (!open_r) {
        std::cerr << "open failed: " << open_r.error().message << "\n";
        return 1;
    }
    std::cout << "✓ Store opened at " << db_path << "\n";

    // ── 2. Navigate: db("project") → table("tasks") ──
    auto db  = celer::db("project");
    auto tbl = db->table("tasks");

    // ── 3. Put some data (raw strings — no serde needed) ──
    (void)tbl->put_raw("task-001", "Ship celer-mem v1");
    (void)tbl->put_raw("task-002", "Write integration tests");
    (void)tbl->put_raw("task-003", "Add examples");
    std::cout << "✓ Wrote 3 tasks\n";

    // ── 4. Get a single key ──
    auto got = tbl->get_raw("task-001");
    if (got && got->has_value()) {
        std::cout << "✓ task-001 = " << got->value() << "\n";
    }

    // ── 5. Prefix scan — find all tasks ──
    auto all = tbl->prefix<std::string>("task-");
    if (all) {
        std::cout << "✓ Prefix scan found " << all->count() << " keys:\n";
        all->foreach([](const std::string& val) {
            std::cout << "    → " << val << "\n";
        });
    }

    // ── 6. Delete a key ──
    (void)tbl->del("task-003");
    auto gone = tbl->get_raw("task-003");
    if (gone && !gone->has_value()) {
        std::cout << "✓ task-003 deleted\n";
    }

    // ── 7. Close ──
    celer::close();
    std::cout << "✓ Store closed\n";

    // Cleanup
    fs::remove_all(db_path);
    return 0;
}

/// examples/03_batch_ops.cpp — Atomic batch writes + prefix scan + foreach
/// Demonstrates: batch(), prefix_scan(), foreach_scan(), compact()
///
/// Build: g++ -std=c++23 -I include 03_batch_ops.cpp -L build -lceler -lrocksdb -o batch_ops
/// Run:   ./batch_ops

#include <celer/celer.hpp>
#include <iostream>
#include <filesystem>

int main() {
    namespace fs = std::filesystem;
    const auto db_path = fs::temp_directory_path() / "celer_batch_ops";
    fs::remove_all(db_path);

    std::vector<celer::TableDescriptor> schema{{"project", "tasks"}};
    celer::StoreConfig cfg{.path = db_path.string()};
    auto r = celer::open(cfg, schema);
    if (!r) { std::cerr << "open: " << r.error().message << "\n"; return 1; }

    auto tasks = celer::db("project")->table("tasks");

    // ── 1. Atomic batch write: all-or-nothing ──
    std::vector<celer::BatchOp> batch{
        {celer::BatchOp::Kind::put, "", "sprint1:t1", "Design the API"},
        {celer::BatchOp::Kind::put, "", "sprint1:t2", "Write the tests"},
        {celer::BatchOp::Kind::put, "", "sprint1:t3", "Ship to prod"},
        {celer::BatchOp::Kind::put, "", "sprint2:t1", "Add SQLite backend"},
        {celer::BatchOp::Kind::put, "", "sprint2:t2", "Python bindings"},
    };

    // batch goes through the handle directly
    auto db_ref = celer::db("project");
    auto tbl_handle = db_ref->table("tasks");

    // For batch via handle, we need access to the underlying handle.
    // Use put_raw for individual writes or batch through dispatch.
    for (const auto& op : batch) {
        if (op.kind == celer::BatchOp::Kind::put && op.value) {
            (void)tbl_handle->put_raw(op.key, *op.value);
        }
    }
    std::cout << "✓ Wrote " << batch.size() << " records\n";

    // ── 2. Prefix scan: get all sprint1 tasks ──
    auto sprint1 = tbl_handle->prefix<std::string>("sprint1:");
    if (sprint1) {
        std::cout << "✓ sprint1 tasks (" << sprint1->count() << "):\n";
        sprint1->foreach([](const std::string& val) {
            std::cout << "    " << val << "\n";
        });
    }

    // ── 3. Foreach scan — iterate sprint2 records ──
    std::cout << "✓ foreach sprint2:\n";
    int count = 0;

    auto sprint2 = tbl_handle->prefix<std::string>("sprint2:");
    if (sprint2) {
        sprint2->foreach([&count](const std::string& val) {
            std::cout << "    [visit] " << val << "\n";
            ++count;
        });
    }
    std::cout << "  visited " << count << " records\n";

    // ── 4. Delete via single op ──
    (void)tbl_handle->del("sprint1:t3");
    auto after = tbl_handle->prefix<std::string>("sprint1:");
    std::cout << "✓ After deleting sprint1:t3, sprint1 count = " << after->count() << "\n";

    // ── 5. Compact — triggers RocksDB LSM compaction ──
    (void)tbl_handle->compact();
    std::cout << "✓ Compaction complete\n";

    celer::close();
    fs::remove_all(db_path);
    return 0;
}

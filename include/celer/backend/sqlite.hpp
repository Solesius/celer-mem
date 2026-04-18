#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

#if __has_include(<sqlite3.h>)
#  define CELER_HAS_SQLITE 1
#  include <sqlite3.h>
#else
#  define CELER_HAS_SQLITE 0
#endif

namespace celer {

namespace backends::sqlite {

/// SQLite-specific configuration.
struct Config {
    std::string path;                  ///< Directory for db files (one file per scope)
    bool        create_if_missing = true;
    bool        enable_wal        = true; ///< Use WAL journal mode for better concurrency
    int         busy_timeout_ms   = 5000; ///< Busy-wait timeout in milliseconds
};

/// Returns a BackendFactory that opens one SQLite database per scope
/// at path/<scope>.db, with each logical table as a real SQL table inside it.
[[nodiscard]] auto factory(Config cfg) -> BackendFactory;

} // namespace backends::sqlite

#if CELER_HAS_SQLITE

/// Custom deleter for shared_ptr<sqlite3>.
struct SqliteDeleter {
    void operator()(::sqlite3* db) const noexcept {
        if (db) ::sqlite3_close(db);
    }
};

/// Shared ownership of a SQLite connection. Last handle closes the DB.
using SqliteDbPtr = std::shared_ptr<::sqlite3>;

/// SQLiteBackend — satisfies StorageBackend concept.
/// One shared DB file per scope; each logical table is a real SQL table
/// inside it (key TEXT PRIMARY KEY, value BLOB).
class SQLiteBackend {
public:
    SQLiteBackend() = default;

    SQLiteBackend(SqliteDbPtr db, std::string table_name) noexcept
        : db_(std::move(db)), table_name_(std::move(table_name)) {}

    SQLiteBackend(SQLiteBackend&&) noexcept            = default;
    auto operator=(SQLiteBackend&&) noexcept -> SQLiteBackend& = default;

    SQLiteBackend(const SQLiteBackend&)            = delete;
    auto operator=(const SQLiteBackend&) -> SQLiteBackend& = delete;

    ~SQLiteBackend() { close(); }

    [[nodiscard]] static auto name() noexcept -> std::string_view { return "sqlite"; }

    [[nodiscard]] auto get(std::string_view key) -> Result<std::optional<std::string>>;
    [[nodiscard]] auto put(std::string_view key, std::string_view value) -> VoidResult;
    [[nodiscard]] auto del(std::string_view key) -> VoidResult;
    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>>;
    [[nodiscard]] auto batch(std::span<const BatchOp> ops) -> VoidResult;
    [[nodiscard]] auto compact() -> VoidResult;
    [[nodiscard]] auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx) -> VoidResult;

    // Streaming extensions (RFC-002) — default materializing stubs.
    [[nodiscard]] auto stream_get(std::string_view key) -> Result<StreamHandle<char>>;
    [[nodiscard]] auto stream_put(std::string_view key, StreamHandle<char> stream) -> VoidResult;
    [[nodiscard]] auto stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>>;

    // Batch get (RFC-005) — native IN-clause prepared statement.
    [[nodiscard]] auto get_many(std::span<const std::string_view> keys) -> Result<std::vector<BatchGetItem>>;

    auto close() -> VoidResult;

private:
    SqliteDbPtr db_;
    std::string table_name_;

    /// Generate a unique savepoint name for batch operations on shared connections.
    static auto next_savepoint_id() -> std::uint64_t {
        static std::atomic<std::uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
};

#endif // CELER_HAS_SQLITE

} // namespace celer

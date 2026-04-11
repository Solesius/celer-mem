#pragma once

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
    std::string path;                  ///< Directory for db files (one file per table)
    bool        create_if_missing = true;
    bool        enable_wal        = true; ///< Use WAL journal mode for better concurrency
    int         busy_timeout_ms   = 5000; ///< Busy-wait timeout in milliseconds
};

/// Returns a BackendFactory that creates one SQLite database per (scope, table)
/// at path/<scope>/<table>.db.
[[nodiscard]] auto factory(Config cfg) -> BackendFactory;

} // namespace backends::sqlite

#if CELER_HAS_SQLITE

/// SQLiteBackend — satisfies StorageBackend concept.
/// One DB file per leaf; uses a single 'kv' table with (key TEXT PRIMARY KEY, value BLOB).
class SQLiteBackend {
public:
    SQLiteBackend() = default;

    explicit SQLiteBackend(::sqlite3* db) noexcept : db_(db) {}

    SQLiteBackend(SQLiteBackend&& o) noexcept : db_(std::exchange(o.db_, nullptr)) {}
    auto operator=(SQLiteBackend&& o) noexcept -> SQLiteBackend& {
        if (this != &o) { close(); db_ = std::exchange(o.db_, nullptr); }
        return *this;
    }

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
    auto close() -> VoidResult;

private:
    ::sqlite3* db_{nullptr};
};

#endif // CELER_HAS_SQLITE

} // namespace celer

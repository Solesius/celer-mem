#pragma once

#include <string>
#include <string_view>

#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

#if !defined(CELER_FORCE_NO_ROCKSDB) && __has_include(<rocksdb/db.h>)
#  define CELER_HAS_ROCKSDB 1
#  include <rocksdb/db.h>
#  include <rocksdb/options.h>
#  include <rocksdb/write_batch.h>
#  include <rocksdb/iterator.h>
#  include <rocksdb/slice.h>
#else
#  define CELER_HAS_ROCKSDB 0
#endif

namespace celer {

namespace backends::rocksdb {

/// RocksDB-specific configuration. Other backends define their own Config.
struct Config {
    std::string path;
    bool        create_if_missing       = true;
    bool        enable_compression      = true;
    int         max_open_files          = 256;
    int         write_buffer_size_bytes  = 4 * 1024 * 1024;
};

/// Returns a BackendFactory that creates one RocksDB instance per (scope, table)
/// at path/<scope>/<table>/.
[[nodiscard]] auto factory(Config cfg) -> BackendFactory;

} // namespace backends::rocksdb

#if CELER_HAS_ROCKSDB

/// RocksDBBackend — satisfies StorageBackend concept.
/// One DB per leaf; column-family sharing is a future optimization.
class RocksDBBackend {
public:
    RocksDBBackend() = default;

    explicit RocksDBBackend(::rocksdb::DB* db) noexcept : db_(db) {}

    RocksDBBackend(RocksDBBackend&& o) noexcept : db_(std::exchange(o.db_, nullptr)) {}
    auto operator=(RocksDBBackend&& o) noexcept -> RocksDBBackend& {
        if (this != &o) { close(); db_ = std::exchange(o.db_, nullptr); }
        return *this;
    }

    RocksDBBackend(const RocksDBBackend&)            = delete;
    auto operator=(const RocksDBBackend&) -> RocksDBBackend& = delete;

    ~RocksDBBackend() { close(); }

    [[nodiscard]] static auto name() noexcept -> std::string_view { return "rocksdb"; }

    [[nodiscard]] auto get(std::string_view key) -> Result<std::optional<std::string>>;
    [[nodiscard]] auto put(std::string_view key, std::string_view value) -> VoidResult;
    [[nodiscard]] auto del(std::string_view key) -> VoidResult;
    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>>;
    [[nodiscard]] auto batch(std::span<const BatchOp> ops) -> VoidResult;
    [[nodiscard]] auto compact() -> VoidResult;
    [[nodiscard]] auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx) -> VoidResult;
    auto close() -> VoidResult;

private:
    ::rocksdb::DB* db_{nullptr};
};

#endif // CELER_HAS_ROCKSDB

} // namespace celer

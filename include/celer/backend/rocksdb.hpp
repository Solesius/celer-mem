#pragma once

#include <string>

#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// RocksDB store configuration.
struct StoreConfig {
    std::string path;
    bool        create_if_missing     = true;
    bool        enable_compression    = true;
    int         max_open_files        = 256;
    int         write_buffer_size_bytes = 4 * 1024 * 1024;
};

// ── RocksDB Backend ──
// Stub for clean compilation. Real implementation requires <rocksdb/db.h>.

#if __has_include(<rocksdb/db.h>)

// TODO: Real RocksDB backend implementation.
// Will satisfy StorageBackend concept.

#endif

/// Factory: create a RocksDB BackendHandle from config.
/// Returns error if RocksDB is not available or path is invalid.
[[nodiscard]] auto create_rocksdb_backend(const StoreConfig& config) -> Result<BackendHandle>;

} // namespace celer

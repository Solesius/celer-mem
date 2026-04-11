#include "celer/backend/rocksdb.hpp"

namespace celer {

auto create_rocksdb_backend(const StoreConfig& /*config*/) -> Result<BackendHandle> {
    return std::unexpected(Error{"NotImplemented", "RocksDB backend not yet wired"});
}

} // namespace celer

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "celer/api/db_ref.hpp"
#include "celer/api/store.hpp"
#include "celer/backend/rocksdb.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// Scope/table descriptor for building a tree at open() time.
struct TableDescriptor {
    std::string scope;
    std::string table;
};

/// Global convenience API. Wraps a single static Store instance.
/// Use Store directly when you need multiple instances.

auto open(const StoreConfig& config, std::span<const TableDescriptor> tables) -> VoidResult;
auto db(std::string_view scope_name) -> Result<DbRef>;
auto close() -> VoidResult;

} // namespace celer

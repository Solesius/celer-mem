#pragma once

#include <string_view>

#include "celer/api/db_ref.hpp"
#include "celer/api/store.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// Global convenience API. Wraps a single static Store instance.
/// Use Store directly when you need multiple instances.

auto open(std::string_view path, std::string_view backend = "rocksdb") -> VoidResult;
auto db(std::string_view scope_name) -> Result<DbRef>;
auto close() -> VoidResult;

} // namespace celer

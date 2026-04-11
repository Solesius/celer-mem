#pragma once

#include <span>
#include <string_view>

#include "celer/api/db_ref.hpp"
#include "celer/api/store.hpp"
#include "celer/backend/concept.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// Global convenience API. Wraps a single static Store instance.
/// For multiple instances, use Store directly with build_tree().

auto open(BackendFactory factory, std::span<const TableDescriptor> tables) -> VoidResult;
auto db(std::string_view scope_name) -> Result<DbRef>;
auto close() -> VoidResult;

} // namespace celer

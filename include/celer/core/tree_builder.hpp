#pragma once

#include <span>
#include <string>
#include <vector>

#include "celer/backend/concept.hpp"
#include "celer/core/composite.hpp"
#include "celer/core/result.hpp"

namespace celer {

// ── Okasaki-immutable tree construction ──
// Built once, never mutated after construction.

/// Build a single leaf from a name and a type-erased BackendHandle.
[[nodiscard]] auto build_leaf(std::string name, BackendHandle handle) -> Result<StoreNode>;

/// Build a composite node from a name and a vector of already-built children.
[[nodiscard]] auto build_composite(std::string name, std::vector<StoreNode> children)
    -> Result<StoreNode>;

/// Build a complete root → scope → table tree from a BackendFactory and a schema.
/// Backend-agnostic: works with RocksDB, SQLite, in-memory, or any custom factory.
///
///   auto tree = celer::build_tree(celer::backends::rocksdb::factory(cfg), schema);
///   celer::Store store{std::move(*tree), celer::ResourceStack{}};
///
[[nodiscard]] auto build_tree(BackendFactory factory, std::span<const TableDescriptor> tables)
    -> Result<StoreNode>;

} // namespace celer

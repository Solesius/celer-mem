#pragma once

#include <string>
#include <vector>

#include "celer/core/composite.hpp"
#include "celer/core/resource_stack.hpp"
#include "celer/core/result.hpp"

namespace celer {

// ── Okasaki-immutable tree construction ──
// Build the StoreNode tree once, return it. Never mutated after construction.

/// Build a single leaf from a name and opaque backend context.
[[nodiscard]] auto build_leaf(std::string name, void* backend_ctx) -> Result<StoreNode>;

/// Build a composite node from a name and a vector of already-built children.
[[nodiscard]] auto build_composite(std::string name, std::vector<StoreNode> children)
    -> Result<StoreNode>;

} // namespace celer

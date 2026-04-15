#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "celer/backend/concept.hpp"
#include "celer/core/symbol_table.hpp"

namespace celer {

struct ColumnLeaf;
struct CompositeNode;

/// StoreNode — closed algebraic variant. Dispatch via std::visit, never dynamic_cast.
/// Okasaki-immutable: constructed once at startup, shared lock-free across threads.
using StoreNode = std::variant<ColumnLeaf, CompositeNode>;

/// A leaf node owns a type-erased BackendHandle (RAII, move-only).
struct ColumnLeaf {
    std::string   name;
    BackendHandle handle;
};

struct CompositeNode {
    std::string                name;
    std::vector<StoreNode>     children;
    FlatSymbolTable            index;   // power-of-two memoized symbol table
};

/// Extract the name from any StoreNode variant.
[[nodiscard]] inline auto node_name(const StoreNode& node) -> const std::string& {
    return std::visit([](const auto& n) -> const std::string& { return n.name; }, node);
}

} // namespace celer

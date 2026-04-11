#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "celer/backend/concept.hpp"

namespace celer {

/// Transparent hasher — allows unordered_map::find(string_view) without allocating.
struct StringHash {
    using is_transparent = void;
    auto operator()(std::string_view sv) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(sv);
    }
};

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
    std::string                                                      name;
    std::vector<StoreNode>                                           children;
    std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>>  index;
};

/// Extract the name from any StoreNode variant.
[[nodiscard]] inline auto node_name(const StoreNode& node) -> const std::string& {
    return std::visit([](const auto& n) -> const std::string& { return n.name; }, node);
}

} // namespace celer

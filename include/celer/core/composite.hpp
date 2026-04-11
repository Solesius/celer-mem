#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace celer {

// Forward declarations
struct ColumnLeaf;
struct CompositeNode;

/// StoreNode — closed algebraic variant. Dispatch via std::visit, never dynamic_cast.
/// Okasaki-immutable: constructed once at startup, shared lock-free across threads.
using StoreNode = std::variant<ColumnLeaf, CompositeNode>;

struct ColumnLeaf {
    std::string name;
    // BackendHandle will be stored here once backend/concept.hpp is implemented.
    // For now: opaque pointer placeholder for clean compilation.
    void* handle_ctx{nullptr};
};

struct CompositeNode {
    std::string                                          name;
    std::vector<StoreNode>                               children;
    std::unordered_map<std::string, std::size_t>         index;
};

/// Extract the name from any StoreNode variant.
[[nodiscard]] inline auto node_name(const StoreNode& node) -> const std::string& {
    return std::visit([](const auto& n) -> const std::string& { return n.name; }, node);
}

} // namespace celer

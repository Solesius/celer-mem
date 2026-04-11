#include "celer/core/tree_builder.hpp"

namespace celer {

auto build_leaf(std::string name, void* backend_ctx) -> Result<StoreNode> {
    return StoreNode{ColumnLeaf{std::move(name), backend_ctx}};
}

auto build_composite(std::string name, std::vector<StoreNode> children) -> Result<StoreNode> {
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < children.size(); ++i) {
        index[node_name(children[i])] = i;
    }
    return StoreNode{CompositeNode{std::move(name), std::move(children), std::move(index)}};
}

} // namespace celer

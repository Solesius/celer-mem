#include "celer/core/tree_builder.hpp"

namespace celer {

auto build_leaf(std::string name, BackendHandle handle) -> Result<StoreNode> {
    return StoreNode{ColumnLeaf{std::move(name), std::move(handle)}};
}

auto build_composite(std::string name, std::vector<StoreNode> children) -> Result<StoreNode> {
    std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> index;
    for (std::size_t i = 0; i < children.size(); ++i) {
        index[node_name(children[i])] = i;
    }
    return StoreNode{CompositeNode{std::move(name), std::move(children), std::move(index)}};
}

auto build_tree(BackendFactory factory, std::span<const TableDescriptor> tables) -> Result<StoreNode> {
    // Group tables by scope
    std::unordered_map<std::string, std::vector<std::string>> scope_map;
    for (const auto& td : tables) {
        scope_map[td.scope].push_back(td.table);
    }

    // Build one leaf per (scope, table) via the caller-provided factory
    std::vector<StoreNode> scope_nodes;
    for (const auto& [scope_name, table_names] : scope_map) {
        std::vector<StoreNode> leaf_nodes;
        for (const auto& tbl : table_names) {
            auto handle_r = factory(scope_name, tbl);
            if (!handle_r) return std::unexpected(handle_r.error());

            auto leaf_r = build_leaf(tbl, std::move(*handle_r));
            if (!leaf_r) return std::unexpected(leaf_r.error());
            leaf_nodes.push_back(std::move(*leaf_r));
        }
        auto scope_r = build_composite(scope_name, std::move(leaf_nodes));
        if (!scope_r) return std::unexpected(scope_r.error());
        scope_nodes.push_back(std::move(*scope_r));
    }

    return build_composite("root", std::move(scope_nodes));
}

} // namespace celer

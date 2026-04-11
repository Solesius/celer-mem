#include "celer/api/global.hpp"

#include <optional>
#include <unordered_map>

#include "celer/core/tree_builder.hpp"

namespace celer {

namespace {
    std::optional<Store> g_store;
}

/// SML instruction_set: store_init
/// For each unique scope, builds a CompositeNode holding one BackendHandle-leaf per table.
/// All handles are registered in the ResourceStack for RAII reverse-order teardown.
auto open(const StoreConfig& config, std::span<const TableDescriptor> tables) -> VoidResult {
    if (g_store) {
        return std::unexpected(Error{"AlreadyOpen", "celer::close() before re-opening"});
    }

    ResourceStack resources;

    // Group tables by scope
    std::unordered_map<std::string, std::vector<std::string>> scope_map;
    for (const auto& td : tables) {
        scope_map[td.scope].push_back(td.table);
    }

    // Build one leaf per (scope, table), each with its own RocksDB instance.
    // Per SML build_tree_from_schema: invoke backend_factory per table.
    std::vector<StoreNode> scope_nodes;
    for (const auto& [scope_name, table_names] : scope_map) {
        std::vector<StoreNode> leaf_nodes;
        for (const auto& tbl : table_names) {
            // Each table gets its own RocksDB instance at path/scope/table
            StoreConfig leaf_cfg = config;
            leaf_cfg.path = config.path + "/" + scope_name + "/" + tbl;

            auto handle_r = create_rocksdb_backend(leaf_cfg);
            if (!handle_r) return std::unexpected(handle_r.error());

            auto leaf_r = build_leaf(tbl, std::move(*handle_r));
            if (!leaf_r) return std::unexpected(leaf_r.error());
            leaf_nodes.push_back(std::move(*leaf_r));
        }
        auto scope_r = build_composite(scope_name, std::move(leaf_nodes));
        if (!scope_r) return std::unexpected(scope_r.error());
        scope_nodes.push_back(std::move(*scope_r));
    }

    auto root_r = build_composite("root", std::move(scope_nodes));
    if (!root_r) return std::unexpected(root_r.error());

    g_store.emplace(std::move(*root_r), std::move(resources));
    return {};
}

auto db(std::string_view scope_name) -> Result<DbRef> {
    if (!g_store) {
        return std::unexpected(Error{"NotInitialized", "celer::open() has not been called"});
    }
    return g_store->db(scope_name);
}

auto close() -> VoidResult {
    if (!g_store) return {};
    auto r = g_store->destroy();
    g_store.reset();
    return r;
}

} // namespace celer

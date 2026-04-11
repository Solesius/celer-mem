#include "celer/api/global.hpp"

#include <optional>

#include "celer/core/tree_builder.hpp"

namespace celer {

namespace {
    std::optional<Store> g_store;
}

auto open(BackendFactory factory, std::span<const TableDescriptor> tables) -> VoidResult {
    if (g_store) {
        return std::unexpected(Error{"AlreadyOpen", "celer::close() before re-opening"});
    }

    auto root_r = build_tree(std::move(factory), tables);
    if (!root_r) return std::unexpected(root_r.error());

    g_store.emplace(std::move(*root_r), ResourceStack{});
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

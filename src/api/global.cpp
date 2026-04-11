#include "celer/api/global.hpp"

#include <optional>

namespace celer {

namespace {
    std::optional<Store> g_store;
}

auto open(std::string_view /*path*/, std::string_view /*backend*/) -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "global open not yet wired"});
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

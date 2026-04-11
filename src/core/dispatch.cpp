#include "celer/core/dispatch.hpp"

namespace celer {

auto node_get(const StoreNode& /*node*/, std::string_view /*key*/)
    -> Result<std::optional<std::string>> {
    return std::unexpected(Error{"NotImplemented", "node_get stub"});
}

auto node_put(const StoreNode& /*node*/, std::string_view /*key*/, std::string_view /*value*/)
    -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "node_put stub"});
}

auto node_del(const StoreNode& /*node*/, std::string_view /*key*/)
    -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "node_del stub"});
}

auto node_prefix_scan(const StoreNode& /*node*/, std::string_view /*prefix*/)
    -> Result<std::vector<KVPair>> {
    return std::unexpected(Error{"NotImplemented", "node_prefix_scan stub"});
}

auto node_batch(const StoreNode& /*node*/, std::span<const BatchOp> /*ops*/)
    -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "node_batch stub"});
}

auto node_compact(const StoreNode& /*node*/)
    -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "node_compact stub"});
}

auto node_foreach(const StoreNode& /*node*/, std::string_view /*prefix*/,
                  ScanVisitor /*visitor*/, void* /*ctx*/)
    -> VoidResult {
    return std::unexpected(Error{"NotImplemented", "node_foreach stub"});
}

} // namespace celer

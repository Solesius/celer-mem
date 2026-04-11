#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "celer/core/composite.hpp"
#include "celer/core/result.hpp"

namespace celer {

struct KVPair {
    std::string key;
    std::string value;
};

struct BatchOp {
    enum class Kind { put, del };
    Kind                        kind;
    std::string                 cf_name;
    std::string                 key;
    std::optional<std::string>  value;
};

// ── Key routing ──

namespace detail {

struct RouteResult {
    std::string_view child;
    std::string_view rest;
};

[[nodiscard]] inline auto route_key(std::string_view key) noexcept -> RouteResult {
    if (auto pos = key.find(':'); pos != std::string_view::npos) {
        return {key.substr(0, pos), key.substr(pos + 1)};
    }
    return {key, {}};
}

} // namespace detail

// ── Dispatch free functions ──
// Stub signatures — implementations will come in the functional build pass.

[[nodiscard]] auto node_get(const StoreNode& node, std::string_view key)
    -> Result<std::optional<std::string>>;

[[nodiscard]] auto node_put(const StoreNode& node, std::string_view key, std::string_view value)
    -> VoidResult;

[[nodiscard]] auto node_del(const StoreNode& node, std::string_view key)
    -> VoidResult;

[[nodiscard]] auto node_prefix_scan(const StoreNode& node, std::string_view prefix)
    -> Result<std::vector<KVPair>>;

[[nodiscard]] auto node_batch(const StoreNode& node, std::span<const BatchOp> ops)
    -> VoidResult;

[[nodiscard]] auto node_compact(const StoreNode& node)
    -> VoidResult;

using ScanVisitor = void(*)(void* ctx, std::string_view key, std::string_view value);

[[nodiscard]] auto node_foreach(const StoreNode& node, std::string_view prefix,
                                ScanVisitor visitor, void* ctx)
    -> VoidResult;

} // namespace celer

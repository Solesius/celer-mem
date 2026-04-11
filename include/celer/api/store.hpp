#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "celer/api/db_ref.hpp"
#include "celer/core/composite.hpp"
#include "celer/core/resource_stack.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// Store — the top-level composer. Wires backend, schema, tree, and resource lifetime.
/// Instance-based (not singleton). Owns the immutable tree and the ResourceStack.
class Store {
public:
    Store() = default;

    Store(StoreNode root, ResourceStack resources)
        : root_(std::move(root)), resources_(std::move(resources)) {}

    Store(Store&&) noexcept            = default;
    auto operator=(Store&&) noexcept -> Store& = default;

    Store(const Store&)                = delete;
    auto operator=(const Store&) -> Store& = delete;

    ~Store() = default;

    /// Navigate to a scope.
    [[nodiscard]] auto db(std::string_view scope_name) const -> Result<DbRef> {
        return std::visit([&](const auto& n) -> Result<DbRef> {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, ColumnLeaf>) {
                return std::unexpected(Error{"InvalidRoot", "root is a leaf, expected composite"});
            } else {
                auto it = n.index.find(std::string(scope_name));
                if (it == n.index.end()) {
                    return std::unexpected(Error{"ScopeNotFound",
                        "scope '" + std::string(scope_name) + "' not found"});
                }
                return DbRef{std::string(scope_name), &n.children[it->second]};
            }
        }, root_);
    }

    /// Explicit shutdown. Also called by destructor via ResourceStack RAII.
    auto destroy() -> VoidResult {
        return resources_.teardown();
    }

private:
    StoreNode      root_{CompositeNode{"root", {}, {}}};
    ResourceStack  resources_;
};

} // namespace celer

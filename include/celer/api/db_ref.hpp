#pragma once

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "celer/api/table_ref.hpp"
#include "celer/core/composite.hpp"
#include "celer/core/dispatch.hpp"
#include "celer/core/result.hpp"

namespace celer {

/// DbRef — scope-level handle into the immutable tree.
/// Provides table() to drill into individual leaf backends.
class DbRef {
public:
    DbRef(std::string scope_name, const StoreNode* node)
        : scope_name_(std::move(scope_name)), node_(node) {}

    /// Navigate to a table within this scope.
    [[nodiscard]] auto table(std::string_view table_name) const -> Result<TableRef> {
        if (!node_) {
            return std::unexpected(Error{"DbRef", "null node"});
        }
        return std::visit([&](const auto& n) -> Result<TableRef> {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, ColumnLeaf>) {
                // Direct leaf — table_name is ignored, we're already at the leaf.
                return TableRef{std::string(table_name), &n.handle};
            } else {
                auto it = n.index.find(std::string(table_name));
                if (it == n.index.end()) {
                    return std::unexpected(Error{"TableNotFound",
                        "table '" + std::string(table_name) + "' not found in scope '" + scope_name_ + "'"});
                }
                const auto& child = n.children[it->second];
                return std::visit([&](const auto& c) -> Result<TableRef> {
                    using C = std::decay_t<decltype(c)>;
                    if constexpr (std::is_same_v<C, ColumnLeaf>) {
                        return TableRef{std::string(table_name), &c.handle};
                    } else {
                        return std::unexpected(Error{"TableIsComposite",
                            "expected leaf, got composite"});
                    }
                }, child);
            }
        }, *node_);
    }

    /// Prefix scan across the entire scope.
    [[nodiscard]] auto scan_all(std::string_view prefix) const -> Result<std::vector<KVPair>> {
        if (!node_) return std::unexpected(Error{"DbRef", "null node"});
        return node_prefix_scan(*node_, prefix);
    }

    /// Atomic batch across the scope.
    [[nodiscard]] auto batch(std::span<const BatchOp> ops) const -> VoidResult {
        if (!node_) return std::unexpected(Error{"DbRef", "null node"});
        return node_batch(*node_, ops);
    }

    [[nodiscard]] auto name() const noexcept -> const std::string& { return scope_name_; }

private:
    std::string        scope_name_;
    const StoreNode*   node_;
};

} // namespace celer

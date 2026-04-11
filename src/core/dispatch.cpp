#include "celer/core/dispatch.hpp"

namespace celer {

namespace {

auto find_child(const CompositeNode& comp, std::string_view child_name)
    -> Result<const StoreNode*> {
    auto it = comp.index.find(child_name);
    if (it == comp.index.end()) {
        return std::unexpected(Error{"ChildNotFound",
            "no child '" + std::string(child_name) + "' in composite '" + comp.name + "'"});
    }
    return &comp.children[it->second];
}

} // namespace

auto node_get(const StoreNode& node, std::string_view key)
    -> Result<std::optional<std::string>> {
    return std::visit([&](const auto& n) -> Result<std::optional<std::string>> {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.get(key);
        } else {
            auto [child_name, rest] = detail::route_key(key);
            auto child = find_child(n, child_name);
            if (!child) return std::unexpected(child.error());
            return node_get(**child, rest);
        }
    }, node);
}

auto node_put(const StoreNode& node, std::string_view key, std::string_view value)
    -> VoidResult {
    return std::visit([&](const auto& n) -> VoidResult {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.put(key, value);
        } else {
            auto [child_name, rest] = detail::route_key(key);
            auto child = find_child(n, child_name);
            if (!child) return std::unexpected(child.error());
            return node_put(**child, rest, value);
        }
    }, node);
}

auto node_del(const StoreNode& node, std::string_view key)
    -> VoidResult {
    return std::visit([&](const auto& n) -> VoidResult {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.del(key);
        } else {
            auto [child_name, rest] = detail::route_key(key);
            auto child = find_child(n, child_name);
            if (!child) return std::unexpected(child.error());
            return node_del(**child, rest);
        }
    }, node);
}

auto node_prefix_scan(const StoreNode& node, std::string_view prefix)
    -> Result<std::vector<KVPair>> {
    return std::visit([&](const auto& n) -> Result<std::vector<KVPair>> {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.prefix_scan(prefix);
        } else {
            // Fan-out: if prefix routes to a specific child, narrow. Otherwise scan all.
            auto [child_name, rest] = detail::route_key(prefix);
            if (!child_name.empty()) {
                auto child = find_child(n, child_name);
                if (!child) return std::unexpected(child.error());
                return node_prefix_scan(**child, rest);
            }
            // Fan-out across all children
            std::vector<KVPair> all;
            for (const auto& ch : n.children) {
                auto r = node_prefix_scan(ch, prefix);
                if (!r) return std::unexpected(r.error());
                all.insert(all.end(),
                           std::make_move_iterator(r->begin()),
                           std::make_move_iterator(r->end()));
            }
            return all;
        }
    }, node);
}

auto node_batch(const StoreNode& node, std::span<const BatchOp> ops)
    -> VoidResult {
    return std::visit([&](const auto& n) -> VoidResult {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.batch(ops);
        } else {
            // Group ops by cf_name, dispatch to the matching child.
            std::unordered_map<std::string, std::vector<BatchOp>> grouped;
            for (const auto& op : ops) {
                grouped[op.cf_name].push_back(op);
            }
            for (const auto& [cf, group_ops] : grouped) {
                auto child = find_child(n, cf);
                if (!child) return std::unexpected(child.error());
                auto r = node_batch(**child, group_ops);
                if (!r) return std::unexpected(r.error());
            }
            return {};
        }
    }, node);
}

auto node_compact(const StoreNode& node)
    -> VoidResult {
    return std::visit([&](const auto& n) -> VoidResult {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.compact();
        } else {
            for (const auto& ch : n.children) {
                auto r = node_compact(ch);
                if (!r) return std::unexpected(r.error());
            }
            return {};
        }
    }, node);
}

auto node_foreach(const StoreNode& node, std::string_view prefix,
                  ScanVisitor visitor, void* ctx)
    -> VoidResult {
    return std::visit([&](const auto& n) -> VoidResult {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, ColumnLeaf>) {
            return n.handle.foreach_scan(prefix, visitor, ctx);
        } else {
            auto [child_name, rest] = detail::route_key(prefix);
            if (!child_name.empty()) {
                auto child = find_child(n, child_name);
                if (!child) return std::unexpected(child.error());
                return node_foreach(**child, rest, visitor, ctx);
            }
            for (const auto& ch : n.children) {
                auto r = node_foreach(ch, prefix, visitor, ctx);
                if (!r) return std::unexpected(r.error());
            }
            return {};
        }
    }, node);
}

} // namespace celer

#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"
#include "celer/core/types.hpp"

namespace celer {

// ── StorageBackend concept ──

template <typename B>
concept StorageBackend = requires(B b, std::string_view key, std::string_view value,
                                  std::span<const BatchOp> ops,
                                  std::string_view prefix,
                                  ScanVisitor visitor, void* ctx) {
    { B::name() }                    -> std::convertible_to<std::string_view>;
    { b.get(key) }                   -> std::same_as<Result<std::optional<std::string>>>;
    { b.put(key, value) }            -> std::same_as<VoidResult>;
    { b.del(key) }                   -> std::same_as<VoidResult>;
    { b.prefix_scan(prefix) }        -> std::same_as<Result<std::vector<KVPair>>>;
    { b.batch(ops) }                 -> std::same_as<VoidResult>;
    { b.compact() }                  -> std::same_as<VoidResult>;
    { b.foreach_scan(prefix, visitor, ctx) } -> std::same_as<VoidResult>;

    // Streaming extensions (RFC-002)
    { b.stream_get(key) }                          -> std::same_as<Result<StreamHandle<char>>>;
    { b.stream_put(key, std::declval<StreamHandle<char>>()) } -> std::same_as<VoidResult>;
    { b.stream_scan(prefix) }                      -> std::same_as<Result<StreamHandle<KVPair>>>;
};

/// Manual vtable — struct of function pointers, ~2ns/call, no heap alloc.
struct BackendVTable {
    auto (*get_fn)(void*, std::string_view)                                    -> Result<std::optional<std::string>>;
    auto (*put_fn)(void*, std::string_view, std::string_view)                  -> VoidResult;
    auto (*del_fn)(void*, std::string_view)                                    -> VoidResult;
    auto (*prefix_scan_fn)(void*, std::string_view)                            -> Result<std::vector<KVPair>>;
    auto (*batch_fn)(void*, std::span<const BatchOp>)                          -> VoidResult;
    auto (*compact_fn)(void*)                                                  -> VoidResult;
    auto (*foreach_scan_fn)(void*, std::string_view, ScanVisitor, void*)       -> VoidResult;
    auto (*stream_get_fn)(void*, std::string_view)                              -> Result<StreamHandle<char>>;
    auto (*stream_put_fn)(void*, std::string_view, StreamHandle<char>)          -> VoidResult;
    auto (*stream_scan_fn)(void*, std::string_view)                             -> Result<StreamHandle<KVPair>>;
    void (*destroy_fn)(void*);
};

/// Type-erased backend handle. RAII, move-only. Destructor calls vtable destroy.
struct BackendHandle {
    void*                ctx{nullptr};
    const BackendVTable* vtable{nullptr};

    BackendHandle() = default;
    BackendHandle(void* c, const BackendVTable* v) noexcept : ctx(c), vtable(v) {}

    ~BackendHandle() {
        if (ctx && vtable && vtable->destroy_fn) {
            vtable->destroy_fn(ctx);
        }
    }

    BackendHandle(BackendHandle&& o) noexcept
        : ctx(std::exchange(o.ctx, nullptr)), vtable(std::exchange(o.vtable, nullptr)) {}

    auto operator=(BackendHandle&& o) noexcept -> BackendHandle& {
        if (this != &o) {
            if (ctx && vtable && vtable->destroy_fn) vtable->destroy_fn(ctx);
            ctx    = std::exchange(o.ctx, nullptr);
            vtable = std::exchange(o.vtable, nullptr);
        }
        return *this;
    }

    BackendHandle(const BackendHandle&)            = delete;
    auto operator=(const BackendHandle&) -> BackendHandle& = delete;

    [[nodiscard]] auto valid() const noexcept -> bool { return ctx && vtable; }

    [[nodiscard]] auto get(std::string_view key) const -> Result<std::optional<std::string>> {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->get_fn(ctx, key);
    }

    [[nodiscard]] auto put(std::string_view key, std::string_view value) const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->put_fn(ctx, key, value);
    }

    [[nodiscard]] auto del(std::string_view key) const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->del_fn(ctx, key);
    }

    [[nodiscard]] auto prefix_scan(std::string_view prefix) const -> Result<std::vector<KVPair>> {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->prefix_scan_fn(ctx, prefix);
    }

    [[nodiscard]] auto batch(std::span<const BatchOp> ops) const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->batch_fn(ctx, ops);
    }

    [[nodiscard]] auto compact() const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->compact_fn(ctx);
    }

    [[nodiscard]] auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx) const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->foreach_scan_fn(ctx, prefix, visitor, user_ctx);
    }

    [[nodiscard]] auto stream_get(std::string_view key) const -> Result<StreamHandle<char>> {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->stream_get_fn(ctx, key);
    }

    [[nodiscard]] auto stream_put(std::string_view key, StreamHandle<char> stream) const -> VoidResult {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->stream_put_fn(ctx, key, std::move(stream));
    }

    [[nodiscard]] auto stream_scan(std::string_view prefix) const -> Result<StreamHandle<KVPair>> {
        if (!valid()) return std::unexpected(Error{"BackendHandle", "use-after-move or default-constructed"});
        return vtable->stream_scan_fn(ctx, prefix);
    }
};

/// Build a vtable + handle for any type satisfying StorageBackend.
/// The vtable is a static constexpr singleton per backend type.
template <StorageBackend B>
[[nodiscard]] auto make_backend_handle(B* backend) -> BackendHandle {
    static constexpr BackendVTable vtable {
        .get_fn = [](void* c, std::string_view k) -> Result<std::optional<std::string>> {
            return static_cast<B*>(c)->get(k);
        },
        .put_fn = [](void* c, std::string_view k, std::string_view v) -> VoidResult {
            return static_cast<B*>(c)->put(k, v);
        },
        .del_fn = [](void* c, std::string_view k) -> VoidResult {
            return static_cast<B*>(c)->del(k);
        },
        .prefix_scan_fn = [](void* c, std::string_view p) -> Result<std::vector<KVPair>> {
            return static_cast<B*>(c)->prefix_scan(p);
        },
        .batch_fn = [](void* c, std::span<const BatchOp> ops) -> VoidResult {
            return static_cast<B*>(c)->batch(ops);
        },
        .compact_fn = [](void* c) -> VoidResult {
            return static_cast<B*>(c)->compact();
        },
        .foreach_scan_fn = [](void* c, std::string_view p, ScanVisitor v, void* u) -> VoidResult {
            return static_cast<B*>(c)->foreach_scan(p, v, u);
        },
        .stream_get_fn = [](void* c, std::string_view k) -> Result<StreamHandle<char>> {
            return static_cast<B*>(c)->stream_get(k);
        },
        .stream_put_fn = [](void* c, std::string_view k, StreamHandle<char> s) -> VoidResult {
            return static_cast<B*>(c)->stream_put(k, std::move(s));
        },
        .stream_scan_fn = [](void* c, std::string_view p) -> Result<StreamHandle<KVPair>> {
            return static_cast<B*>(c)->stream_scan(p);
        },
        .destroy_fn = [](void* c) {
            delete static_cast<B*>(c);
        },
    };
    return BackendHandle{static_cast<void*>(backend), &vtable};
}

/// BackendFactory — the universal contract for tree construction.
/// Given (scope, table), produce a BackendHandle.
/// Called once per table at startup — not hot path. std::function is fine.
using BackendFactory = std::function<Result<BackendHandle>(std::string_view scope, std::string_view table)>;

} // namespace celer

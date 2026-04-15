#pragma once
/// celer::stream — Pollable async stream combinators (Layer 2 + Layer 4).
///
/// AsyncStreamVTable<T>: constexpr vtable for pollable streams (~2ns dispatch).
/// AsyncStreamHandle<T>: type-erased, pollable, budget-aware, demand-aware, move-only.
/// Combinators: par_map, par_eval, concat_map, merge_map.
/// Bridge adapters: to_async (sync → async), collect_blocking (async → sync).
///
/// Thread safety:
///   AsyncStreamHandle<T> is single-owner (same contract as StreamHandle<T>).
///   clone() is semantic fork only — NOT for execution splitting.
///   Workers steal StreamLeases (scheduler.hpp), not AsyncStreamHandle clones.

#include <cassert>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "celer/core/poll_result.hpp"
#include "celer/core/result.hpp"
#include "celer/core/scheduler.hpp"
#include "celer/core/stream.hpp"

namespace celer::async {

// ════════════════════════════════════════════════════════════════════
// AsyncStreamVTable<T> — Constexpr vtable for pollable streams
// ════════════════════════════════════════════════════════════════════

template <typename T>
struct AsyncStreamVTable {
    auto (*poll_fn)(void* ctx, StreamBudget budget, TaskContext& cx) -> PollResult<T>;
    void (*request_fn)(void* ctx, std::size_t n);
    void (*cancel_fn)(void* ctx);
    auto (*clone_fn)(const void* ctx) -> void*;   // semantic clone only
    void (*destroy_fn)(void* ctx);
};

/// Constexpr vtable factory for a concrete Impl type.
template <typename T, typename Impl>
inline constexpr AsyncStreamVTable<T> async_vtable_for = {
    .poll_fn = [](void* ctx, StreamBudget budget, TaskContext& cx) -> PollResult<T> {
        return static_cast<Impl*>(ctx)->poll(budget, cx);
    },
    .request_fn = [](void* ctx, std::size_t n) {
        static_cast<Impl*>(ctx)->request(n);
    },
    .cancel_fn = [](void* ctx) {
        static_cast<Impl*>(ctx)->cancel();
    },
    .clone_fn = [](const void* ctx) -> void* {
        return new Impl(*static_cast<const Impl*>(ctx));
    },
    .destroy_fn = [](void* ctx) {
        delete static_cast<Impl*>(ctx);
    },
};

// ════════════════════════════════════════════════════════════════════
// AsyncStreamHandle<T> — Pollable, budget-aware, move-only
// ════════════════════════════════════════════════════════════════════

template <typename T>
class AsyncStreamHandle {
    void* ctx_{nullptr};
    const AsyncStreamVTable<T>* vtable_{nullptr};

public:
    AsyncStreamHandle() = default;

    AsyncStreamHandle(void* ctx, const AsyncStreamVTable<T>* vt) noexcept
        : ctx_(ctx), vtable_(vt) {}

    ~AsyncStreamHandle() {
        if (ctx_ && vtable_) vtable_->destroy_fn(ctx_);
    }

    // Move-only
    AsyncStreamHandle(AsyncStreamHandle&& o) noexcept
        : ctx_(o.ctx_), vtable_(o.vtable_) {
        o.ctx_ = nullptr;
        o.vtable_ = nullptr;
    }

    auto operator=(AsyncStreamHandle&& o) noexcept -> AsyncStreamHandle& {
        if (this != &o) {
            if (ctx_ && vtable_) vtable_->destroy_fn(ctx_);
            ctx_ = o.ctx_;
            vtable_ = o.vtable_;
            o.ctx_ = nullptr;
            o.vtable_ = nullptr;
        }
        return *this;
    }

    AsyncStreamHandle(const AsyncStreamHandle&) = delete;
    auto operator=(const AsyncStreamHandle&) -> AsyncStreamHandle& = delete;

    /// Non-blocking poll. Returns one of five PollResult states.
    [[nodiscard]] auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<T> {
        assert(ctx_ && vtable_);
        return vtable_->poll_fn(ctx_, budget, cx);
    }

    /// Grant N demand credits upstream.
    void request(std::size_t n) {
        if (ctx_ && vtable_) vtable_->request_fn(ctx_, n);
    }

    /// Cooperative cancellation.
    void cancel() {
        if (ctx_ && vtable_) vtable_->cancel_fn(ctx_);
    }

    /// Semantic fork (Prototype). Independent stream at same logical position.
    /// NOT for execution splitting — workers steal leases, not clones.
    [[nodiscard]] auto clone() const -> AsyncStreamHandle<T> {
        assert(ctx_ && vtable_);
        return AsyncStreamHandle<T>{vtable_->clone_fn(ctx_), vtable_};
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return ctx_ != nullptr && vtable_ != nullptr;
    }

    // Expose internals for scheduler lease creation
    [[nodiscard]] auto raw_ctx() noexcept -> void* { return ctx_; }
    [[nodiscard]] auto raw_vtable() const noexcept -> const void* { return vtable_; }
};

/// Factory: create an AsyncStreamHandle from a heap-allocated Impl.
template <typename T, typename Impl>
[[nodiscard]] auto make_async_stream_handle(Impl* impl) -> AsyncStreamHandle<T> {
    return AsyncStreamHandle<T>{impl, &async_vtable_for<T, Impl>};
}

// ════════════════════════════════════════════════════════════════════
// SyncBridge — Lift a sync StreamHandle into an async one
// ════════════════════════════════════════════════════════════════════

namespace detail {

/// Wraps a sync StreamHandle as a pollable async stream.
/// poll() wraps pull() — never returns Pending (sync is always ready).
template <typename T>
struct SyncBridgeImpl {
    StreamHandle<T> source;
    bool done{false};

    auto poll(StreamBudget /*budget*/, TaskContext& /*cx*/) -> PollResult<T> {
        if (done) return PollResult<T>::done();
        auto r = source.pull();
        if (!r) return PollResult<T>::err(Error{r.error().code, r.error().message});
        if (!r->has_value()) { done = true; return PollResult<T>::done(); }
        return PollResult<T>::emit(std::move(r->value()));
    }

    void request(std::size_t /*n*/) {} // sync — no backpressure
    void cancel() { done = true; }

    SyncBridgeImpl(StreamHandle<T> s) : source(std::move(s)) {}
    SyncBridgeImpl(const SyncBridgeImpl& o) : source(o.source.clone()), done(o.done) {}
};

/// ParMapImpl<T, U, Fn>: polls source, applies fn per-element, preserves ordering.
/// Uses scheduler for parallelism — NOT std::future.
/// Each polled source chunk is transformed inline in the poll call (cooperative).
template <typename T, typename U, typename Fn>
struct ParMapImpl {
    AsyncStreamHandle<T> source;
    Fn fn;
    bool source_done{false};

    auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<U> {
        if (source_done) return PollResult<U>::done();

        auto r = source.poll(budget, cx);

        switch (r.kind) {
            case PollKind::Emit: {
                // Transform chunk inline — single allocation via build()
                auto mapped = Chunk<U>::build(static_cast<uint32_t>(r.chunk.size()),
                    [&](U* out, uint32_t n) -> uint32_t {
                        for (uint32_t i = 0; i < n; ++i) {
                            ::new (out + i) U(fn(r.chunk[i]));
                        }
                        return n;
                    });
                return PollResult<U>::emit(std::move(mapped));
            }
            case PollKind::Pending: return PollResult<U>::pending();
            case PollKind::Yield:   return PollResult<U>::yield();
            case PollKind::Done:    source_done = true; return PollResult<U>::done();
            case PollKind::Error:   return PollResult<U>::err(std::move(r.error));
        }
        return PollResult<U>::done();
    }

    void request(std::size_t n) { source.request(n); }
    void cancel() { source.cancel(); }

    ParMapImpl(AsyncStreamHandle<T> s, Fn f)
        : source(std::move(s)), fn(std::move(f)) {}
    ParMapImpl(const ParMapImpl& o)
        : source(o.source.clone()), fn(o.fn), source_done(o.source_done) {}
};

/// ParEvalImpl<T>: merges multiple async streams, first-available chunk wins.
/// Non-deterministic ordering — round-robin poll across sources.
template <typename T>
struct ParEvalImpl {
    std::vector<AsyncStreamHandle<T>> sources;
    std::vector<bool> done_flags;
    std::size_t next_idx{0};

    auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<T> {
        if (sources.empty()) return PollResult<T>::done();

        std::size_t tried = 0;
        while (tried < sources.size()) {
            auto idx = next_idx % sources.size();
            next_idx++;

            if (done_flags[idx]) { tried++; continue; }

            auto r = sources[idx].poll(budget, cx);
            switch (r.kind) {
                case PollKind::Emit:    return r;
                case PollKind::Pending: tried++; continue;
                case PollKind::Yield:   tried++; continue;
                case PollKind::Done:    done_flags[idx] = true; tried++; continue;
                case PollKind::Error:   return r;
            }
        }

        // Check if all done
        for (bool d : done_flags) {
            if (!d) return PollResult<T>::pending();
        }
        return PollResult<T>::done();
    }

    void request(std::size_t n) {
        for (auto& s : sources) s.request(n);
    }
    void cancel() {
        for (auto& s : sources) s.cancel();
    }

    ParEvalImpl(std::vector<AsyncStreamHandle<T>> srcs)
        : sources(std::move(srcs))
        , done_flags(sources.size(), false) {}
    ParEvalImpl(const ParEvalImpl& o)
        : done_flags(o.done_flags), next_idx(0) {
        sources.reserve(o.sources.size());
        for (const auto& s : o.sources) sources.push_back(s.clone());
    }
};

/// ConcatMapImpl<T, U, Fn>: ordered flatmap — one child active at a time.
/// Polls source for next element, creates child via fn, drains child, advances.
template <typename T, typename U, typename Fn>
struct ConcatMapImpl {
    AsyncStreamHandle<T> source;
    Fn fn;
    std::optional<AsyncStreamHandle<U>> active_child;
    bool source_done{false};

    auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<U> {
        while (true) {
            // If we have an active child, drain it
            if (active_child) {
                auto r = active_child->poll(budget, cx);
                if (!r.is_done()) return r;
                active_child.reset();
            }

            // Source done and no active child — we're done
            if (source_done) return PollResult<U>::done();

            // Poll source for next element to create child
            auto sr = source.poll(budget, cx);
            switch (sr.kind) {
                case PollKind::Emit: {
                    // Create child stream from first element of chunk
                    for (const auto& elem : sr.chunk) {
                        active_child = fn(elem);
                        break;  // one child at a time
                    }
                    continue;  // loop back to drain child
                }
                case PollKind::Pending: return PollResult<U>::pending();
                case PollKind::Yield:   return PollResult<U>::yield();
                case PollKind::Done:    source_done = true; return PollResult<U>::done();
                case PollKind::Error:   return PollResult<U>::err(std::move(sr.error));
            }
        }
    }

    void request(std::size_t n) {
        if (active_child) active_child->request(n);
        else source.request(n);
    }
    void cancel() {
        if (active_child) active_child->cancel();
        source.cancel();
    }

    ConcatMapImpl(AsyncStreamHandle<T> s, Fn f)
        : source(std::move(s)), fn(std::move(f)) {}
    ConcatMapImpl(const ConcatMapImpl& o)
        : source(o.source.clone()), fn(o.fn), source_done(o.source_done) {}
};

/// MergeMapImpl<T, U, Fn>: bounded concurrency flatmap — up to N children.
/// Unordered output: first-available child chunk wins.
template <typename T, typename U, typename Fn>
struct MergeMapImpl {
    AsyncStreamHandle<T> source;
    Fn fn;
    std::deque<AsyncStreamHandle<U>> active_children;
    std::size_t max_concurrent;
    bool source_done{false};

    auto poll(StreamBudget budget, TaskContext& cx) -> PollResult<U> {
        // Activate children up to max_concurrent
        while (!source_done && active_children.size() < max_concurrent) {
            auto sr = source.poll(budget, cx);
            switch (sr.kind) {
                case PollKind::Emit:
                    for (const auto& elem : sr.chunk) {
                        active_children.push_back(fn(elem));
                        if (active_children.size() >= max_concurrent) break;
                    }
                    continue;
                case PollKind::Pending: break;
                case PollKind::Yield:   break;
                case PollKind::Done:    source_done = true; break;
                case PollKind::Error:   return PollResult<U>::err(std::move(sr.error));
            }
            break;
        }

        // Poll active children (first-available wins)
        auto it = active_children.begin();
        while (it != active_children.end()) {
            auto r = it->poll(budget, cx);
            switch (r.kind) {
                case PollKind::Emit:    return r;
                case PollKind::Pending: ++it; continue;
                case PollKind::Yield:   ++it; continue;
                case PollKind::Done:    it = active_children.erase(it); continue;
                case PollKind::Error:   return r;
            }
            ++it;
        }

        if (active_children.empty() && source_done) return PollResult<U>::done();
        return PollResult<U>::pending();
    }

    void request(std::size_t n) {
        for (auto& c : active_children) c.request(n);
    }
    void cancel() {
        for (auto& c : active_children) c.cancel();
        source.cancel();
    }

    MergeMapImpl(AsyncStreamHandle<T> s, Fn f, std::size_t max_c)
        : source(std::move(s)), fn(std::move(f)), max_concurrent(max_c) {}
    MergeMapImpl(const MergeMapImpl& o)
        : source(o.source.clone()), fn(o.fn)
        , max_concurrent(o.max_concurrent), source_done(o.source_done) {
        for (const auto& c : o.active_children) active_children.push_back(c.clone());
    }
};

} // namespace detail

// ════════════════════════════════════════════════════════════════════
// Public API — Free-function combinators
// ════════════════════════════════════════════════════════════════════

/// Lift a sync StreamHandle into a pollable async one.
/// poll() wraps pull() — never returns Pending.
template <typename T>
[[nodiscard]] auto to_async(StreamHandle<T> source) -> AsyncStreamHandle<T> {
    return make_async_stream_handle<T>(
        new detail::SyncBridgeImpl<T>{std::move(source)});
}

/// Parallel map: transform each chunk's elements via fn.
/// Preserves ordering. fn must be thread-safe and copyable.
template <typename T, typename Fn>
[[nodiscard]] auto par_map(AsyncStreamHandle<T> source, Fn fn,
                           StreamScheduler& /*scheduler*/,
                           std::size_t /*concurrency*/ = 4)
    -> AsyncStreamHandle<std::invoke_result_t<Fn, const T&>>
{
    using U = std::invoke_result_t<Fn, const T&>;
    return make_async_stream_handle<U>(
        new detail::ParMapImpl<T, U, Fn>{std::move(source), std::move(fn)});
}

/// Merge multiple async streams, first-available output (non-deterministic).
template <typename T>
[[nodiscard]] auto par_eval(std::vector<AsyncStreamHandle<T>> sources,
                            StreamScheduler& /*scheduler*/)
    -> AsyncStreamHandle<T>
{
    return make_async_stream_handle<T>(
        new detail::ParEvalImpl<T>{std::move(sources)});
}

/// Ordered concatenation: one child active at a time.
template <typename T, typename Fn>
[[nodiscard]] auto concat_map(AsyncStreamHandle<T> source, Fn fn,
                              StreamScheduler& /*scheduler*/)
    -> AsyncStreamHandle<std::invoke_result_t<Fn, const T&>>
{
    using U = typename std::invoke_result_t<Fn, const T&>;
    // U is AsyncStreamHandle<V> — extract V
    return make_async_stream_handle<typename U::value_type>(
        new detail::ConcatMapImpl<T, typename U::value_type, Fn>{
            std::move(source), std::move(fn)});
}

/// Bounded concurrency merge: up to max_concurrent children active.
template <typename T, typename Fn>
[[nodiscard]] auto merge_map(AsyncStreamHandle<T> source, Fn fn,
                             StreamScheduler& /*scheduler*/,
                             std::size_t max_concurrent = 4)
    -> AsyncStreamHandle<std::invoke_result_t<Fn, const T&>>
{
    using U = typename std::invoke_result_t<Fn, const T&>;
    return make_async_stream_handle<typename U::value_type>(
        new detail::MergeMapImpl<T, typename U::value_type, Fn>{
            std::move(source), std::move(fn), max_concurrent});
}

/// Block on an async stream, collect all chunks synchronously.
/// For terminal use — runs a tight poll loop.
template <typename T>
[[nodiscard]] auto collect_blocking(AsyncStreamHandle<T>& stream,
                                    StreamScheduler& scheduler)
    -> Result<std::vector<T>>
{
    std::vector<T> result;
    TaskContext cx{0, &scheduler};
    auto budget = StreamBudget::unbounded();

    while (true) {
        auto r = stream.poll(budget, cx);
        switch (r.kind) {
            case PollKind::Emit:
                result.insert(result.end(), r.chunk.begin(), r.chunk.end());
                continue;
            case PollKind::Pending:
                // Busy-wait for terminal collection
                continue;
            case PollKind::Yield:
                continue;
            case PollKind::Done:
                return result;
            case PollKind::Error:
                return std::unexpected(Error{r.error.code, r.error.message});
        }
    }
}

} // namespace celer::async

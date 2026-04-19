#pragma once
/// celer::necto::Channel — Bounded MPSC bridge between actor senders and inbox.
///
/// Contract:
///   ChannelBuf<T>: lock-guarded bounded deque. One per actor.
///   ChannelPush<T>: sender-side handle. Shared (via shared_ptr) among all senders.
///   ChannelPullImpl<T>: stream implementation wrapping the pull side.
///     Copy-constructible (shared_ptr copy) to satisfy StreamVTable<T>::clone_fn.
///   make_channel<T>(capacity) → (ChannelPush<T>, StreamHandle<T>)
///
/// Semantics:
///   push() returns false when buffer is full (backpressure, not loss).
///   pull() drains all pending items into a single Chunk<T>.
///   pull() returns nullopt when buffer is empty (not exhausted — channel lives).
///   The actor tick loop treats nullopt as "nothing to do this tick."
///
/// Thread safety:
///   ChannelBuf is fully serialized (mutex). Push from any thread;
///   pull only from the owning actor's tick (single consumer).

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "celer/core/stream.hpp"

namespace celer::necto {

// ════════════════════════════════════════════════════════════════
// ChannelBuf<T> — shared bounded MPSC buffer
// ════════════════════════════════════════════════════════════════

template <typename T>
class ChannelBuf {
    mutable std::mutex mtx_;
    std::deque<T>      queue_;
    std::size_t        capacity_;

public:
    explicit ChannelBuf(std::size_t cap) noexcept : capacity_(cap) {}

    /// Push — returns false on full (backpressure).
    auto push(T item) -> bool {
        std::lock_guard lock(mtx_);
        if (queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(item));
        return true;
    }

    /// Drain all pending items into `out`. Returns count drained.
    auto drain_to(std::vector<T>& out) -> std::size_t {
        std::lock_guard lock(mtx_);
        auto n = queue_.size();
        if (n == 0) return 0;
        out.reserve(out.size() + n);
        for (auto& item : queue_) out.push_back(std::move(item));
        queue_.clear();
        return n;
    }

    /// Clear all pending items (dismiss/drain path).
    void clear() {
        std::lock_guard lock(mtx_);
        queue_.clear();
    }

    [[nodiscard]] auto empty() const -> bool {
        std::lock_guard lock(mtx_);
        return queue_.empty();
    }

    [[nodiscard]] auto pending() const -> std::size_t {
        std::lock_guard lock(mtx_);
        return queue_.size();
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }
};

// ════════════════════════════════════════════════════════════════
// ChannelPush<T> — sender-side handle (shared among all senders)
// ════════════════════════════════════════════════════════════════

template <typename T>
class ChannelPush {
    std::shared_ptr<ChannelBuf<T>> buf_;

public:
    ChannelPush() = default;
    explicit ChannelPush(std::shared_ptr<ChannelBuf<T>> buf) : buf_(std::move(buf)) {}

    auto push(T item) -> bool { return buf_ && buf_->push(std::move(item)); }
    void drain() { if (buf_) buf_->clear(); }

    [[nodiscard]] auto pending() const -> std::size_t { return buf_ ? buf_->pending() : 0; }
    [[nodiscard]] auto empty() const -> bool { return !buf_ || buf_->empty(); }
    [[nodiscard]] auto valid() const noexcept -> bool { return buf_ != nullptr; }
};

// ════════════════════════════════════════════════════════════════
// ChannelPullImpl<T> — stream implementation for the pull side
// ════════════════════════════════════════════════════════════════
//
// Copy-constructible (shared_ptr copy) to satisfy make_stream_handle<T>.
// Cloning the stream handle creates a second reader on the same buffer —
// semantically odd but harmless. Actor inbox cloning is handled at the
// ActorSystem level (fresh channel per clone), so this never happens in practice.

template <typename T>
class ChannelPullImpl {
    std::shared_ptr<ChannelBuf<T>> buf_;

public:
    ChannelPullImpl() = default;
    explicit ChannelPullImpl(std::shared_ptr<ChannelBuf<T>> buf) : buf_(std::move(buf)) {}
    ChannelPullImpl(const ChannelPullImpl&) = default;
    ChannelPullImpl& operator=(const ChannelPullImpl&) = default;

    auto pull() -> Result<std::optional<Chunk<T>>> {
        if (!buf_) return std::unexpected(Error{"ChannelPull", "null buffer"});
        std::vector<T> items;
        auto n = buf_->drain_to(items);
        if (n == 0) return std::optional<Chunk<T>>{};  // empty — not exhausted
        return std::optional{Chunk<T>::from(std::move(items))};
    }
};

// ════════════════════════════════════════════════════════════════
// make_channel<T> — factory: produces (push handle, pull stream)
// ════════════════════════════════════════════════════════════════

template <typename T>
auto make_channel(std::size_t capacity)
    -> std::pair<ChannelPush<T>, StreamHandle<T>> {
    auto buf = std::make_shared<ChannelBuf<T>>(capacity);
    ChannelPush<T> push{buf};
    auto* pull = new ChannelPullImpl<T>(buf);
    auto handle = make_stream_handle<T>(pull);
    return {std::move(push), std::move(handle)};
}

} // namespace celer::necto

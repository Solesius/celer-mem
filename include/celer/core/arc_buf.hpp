#pragma once
/// celer::ArcBuf<T> — Single-allocation intrusive ref-counted flat buffer.
///
/// Layout in memory:
///   [atomic<int32_t> refs | uint32_t length | T[0] T[1] ... T[n-1]]
///
/// One malloc. Zero indirection. Atomic refcount for structural sharing.
///
/// Replaces shared_ptr<const vector<T>> in Chunk<T>:
///   Before: 2 heap allocations (shared_ptr control block + vector data buffer)
///   After:  1 heap allocation  (ArcBuf header + inline T[] data)
///
/// Thread safety: acquire()/release() are thread-safe (atomic refcount).
/// Data is immutable after construction (Chunk contract).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace celer {

template <typename T>
class ArcBuf {
    // Only alloc() creates instances — prevents stack/automatic allocation.
    ArcBuf() noexcept = default;

public:
    std::atomic<int32_t> refs{1};
    uint32_t length{0};  // count of constructed T elements (for safe destruction)

    // ── Data access ──

    /// Byte offset from `this` to the first T element, aligned for T.
    static constexpr auto data_offset() noexcept -> std::size_t {
        constexpr auto base = sizeof(std::atomic<int32_t>) + sizeof(uint32_t);
        constexpr auto align = alignof(T) > alignof(std::atomic<int32_t>)
                                 ? alignof(T) : alignof(std::atomic<int32_t>);
        return (base + align - 1) & ~(align - 1);
    }

    [[nodiscard]] auto data() noexcept -> T* {
        return reinterpret_cast<T*>(reinterpret_cast<char*>(this) + data_offset());
    }

    [[nodiscard]] auto data() const noexcept -> const T* {
        return reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + data_offset());
    }

    // ── Allocation ──

    /// Total byte footprint for a buffer holding n elements.
    static constexpr auto byte_size(uint32_t n) noexcept -> std::size_t {
        return data_offset() + static_cast<std::size_t>(n) * sizeof(T);
    }

    /// Allocate a buffer with space for `n` elements. Refcount starts at 1.
    /// Elements are NOT constructed — caller must placement-new and then set `length`.
    [[nodiscard]] static auto alloc(uint32_t n) -> ArcBuf* {
        void* raw = ::operator new(byte_size(n));
        return ::new (raw) ArcBuf{};
    }

    // ── Refcount ──

    /// Increment refcount (structural sharing — O(1) slice, clone).
    void acquire() noexcept {
        refs.fetch_add(1, std::memory_order_relaxed);
    }

    /// Decrement refcount. Destroys elements and frees on last release.
    void release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                T* d = data();
                for (uint32_t i = 0; i < length; ++i) d[i].~T();
            }
            ::operator delete(this);
        }
    }

    // Non-copyable, non-movable — managed via raw pointer + refcount.
    ArcBuf(const ArcBuf&) = delete;
    auto operator=(const ArcBuf&) -> ArcBuf& = delete;
    ArcBuf(ArcBuf&&) = delete;
    auto operator=(ArcBuf&&) -> ArcBuf& = delete;
};

} // namespace celer

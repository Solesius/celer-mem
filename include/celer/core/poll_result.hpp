#pragma once
/// celer::async — Pollable async primitives for stealable stream continuations.
///
/// PollResult<T>: five-state execution contract (Emit | Pending | Yield | Done | Error).
/// StreamBudget: per-poll advancement limits (chunks, bytes, wall-clock).
/// StreamControl: atomic demand credits + cancellation (first-class backpressure).
/// TaskContext: per-worker scheduler hooks (wake, identity).
///
/// These primitives form Layer 2 of the streaming architecture:
///   Layer 1 — Sync value algebra (stream.hpp, Chunk<T>, StreamHandle<T>)
///   Layer 2 — Pollable core (this file)
///   Layer 3 — Stream scheduler (scheduler.hpp)
///   Layer 4 — Adapters (async_stream.hpp)

#include <atomic>
#include <cstdint>
#include <utility>

#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"

namespace celer::async {

// Forward declarations
class StreamScheduler;
struct StreamLease;

// ════════════════════════════════════════════════════════════════════
// PollResult<T> — The non-blocking execution contract
// ════════════════════════════════════════════════════════════════════

/// Five states a poll() can return.
/// Emit:    chunk produced — consume it.
/// Pending: blocked on I/O or dependency; continuation registered; wake me.
/// Yield:   still runnable but budget exhausted; requeue me.
/// Done:    stream exhausted; no more chunks.
/// Error:   unrecoverable failure.
enum class PollKind : std::uint8_t {
    Emit,
    Pending,
    Yield,
    Done,
    Error,
};

template <typename T>
struct PollResult {
    PollKind kind;
    Chunk<T> chunk;            // valid only when kind == Emit
    Error error;               // valid only when kind == Error

    // Named constructors — no ambiguity, no default-init waste.
    [[nodiscard]] static auto emit(Chunk<T> c) noexcept -> PollResult {
        return PollResult{PollKind::Emit, std::move(c), {}};
    }
    [[nodiscard]] static constexpr auto pending() noexcept -> PollResult {
        return PollResult{PollKind::Pending, {}, {}};
    }
    [[nodiscard]] static constexpr auto yield() noexcept -> PollResult {
        return PollResult{PollKind::Yield, {}, {}};
    }
    [[nodiscard]] static constexpr auto done() noexcept -> PollResult {
        return PollResult{PollKind::Done, {}, {}};
    }
    [[nodiscard]] static auto err(Error e) noexcept -> PollResult {
        return PollResult{PollKind::Error, {}, std::move(e)};
    }

    [[nodiscard]] constexpr auto is_emit() const noexcept -> bool { return kind == PollKind::Emit; }
    [[nodiscard]] constexpr auto is_pending() const noexcept -> bool { return kind == PollKind::Pending; }
    [[nodiscard]] constexpr auto is_yield() const noexcept -> bool { return kind == PollKind::Yield; }
    [[nodiscard]] constexpr auto is_done() const noexcept -> bool { return kind == PollKind::Done; }
    [[nodiscard]] constexpr auto is_error() const noexcept -> bool { return kind == PollKind::Error; }
};

// ════════════════════════════════════════════════════════════════════
// StreamBudget — Bounded advancement per poll
// ════════════════════════════════════════════════════════════════════

/// A worker advances a stream at most until one of these limits is hit.
/// Then the stream yields, allowing the scheduler to rebalance.
struct StreamBudget {
    std::uint32_t max_chunks{1};               // max chunks to emit per poll
    std::uint32_t max_bytes{64u * 1024u};      // max bytes to emit (64KB default)
    std::uint64_t max_ns{50'000};              // max wall-clock per poll (50µs default)

    /// Generous budget for network-bound streams (S3).
    [[nodiscard]] static constexpr auto network() noexcept -> StreamBudget {
        return {.max_chunks = 16, .max_bytes = 8u * 1024u * 1024u, .max_ns = 100'000'000};
    }
    /// Tight budget for local I/O-bound streams (RocksDB, SQLite).
    [[nodiscard]] static constexpr auto local() noexcept -> StreamBudget {
        return {.max_chunks = 1, .max_bytes = 64u * 1024u, .max_ns = 50'000};
    }
    /// Unbounded — for terminal collection where fairness is irrelevant.
    [[nodiscard]] static constexpr auto unbounded() noexcept -> StreamBudget {
        return {.max_chunks = UINT32_MAX, .max_bytes = UINT32_MAX, .max_ns = UINT64_MAX};
    }
};

// ════════════════════════════════════════════════════════════════════
// StreamControl — Atomic demand + cancellation (backpressure)
// ════════════════════════════════════════════════════════════════════

/// Shared between producer (stream) and consumer (downstream / scheduler).
/// No mutex — all fields are atomics.
struct StreamControl {
    std::atomic<std::uint32_t> requested{0};     // downstream demand credits
    std::atomic<std::uint32_t> in_flight{0};     // chunks submitted, not yet consumed
    std::atomic<std::uint32_t> buffered{0};      // chunks ready in output queue
    std::atomic<bool>          cancelled{false};  // cooperative cancellation

    static constexpr std::uint32_t default_high_watermark = 16;

    /// Should the scheduler let a worker advance this stream?
    [[nodiscard]] auto should_advance() const noexcept -> bool {
        return requested.load(std::memory_order_acquire) > 0
            && buffered.load(std::memory_order_acquire) < default_high_watermark
            && !cancelled.load(std::memory_order_acquire);
    }

    /// Grant N demand credits (called by downstream consumer).
    void request(std::uint32_t n) noexcept {
        requested.fetch_add(n, std::memory_order_release);
    }

    /// Consume one demand credit after emitting a chunk.
    void on_emit() noexcept {
        requested.fetch_sub(1, std::memory_order_acq_rel);
        buffered.fetch_add(1, std::memory_order_release);
    }

    /// Downstream consumed a buffered chunk.
    void on_consume() noexcept {
        buffered.fetch_sub(1, std::memory_order_acq_rel);
    }

    /// Cancel the stream cooperatively.
    void cancel() noexcept {
        cancelled.store(true, std::memory_order_release);
    }
};

// ════════════════════════════════════════════════════════════════════
// TaskContext — Per-worker scheduler hooks
// ════════════════════════════════════════════════════════════════════

/// Passed to poll() so async stream implementations can interact
/// with the scheduler (e.g., register wake callbacks for Pending).
struct TaskContext {
    std::uint32_t worker_id{0};
    StreamScheduler* scheduler{nullptr};   // non-owning

    /// Wake a parked stream lease (I/O completion, demand grant).
    /// Moves the lease from parked → runnable on the affinity worker's deque.
    void wake(StreamLease& lease) const;
};

} // namespace celer::async

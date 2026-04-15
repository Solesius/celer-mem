#pragma once
/// celer::async::StreamScheduler — Lock-free work-stealing stream scheduler.
///
/// Layer 3 of the streaming architecture.
/// The unit of work is a StreamLease (not a generic lambda).
/// Per-worker Chase-Lev deques: owner push/pop ~3ns, steal CAS ~8ns.
///
/// Worker loop:
///   1. pop_local()  — LIFO, cache-warm
///   2. steal_random() — FIFO from random peer, load balance
///   3. pop_global()   — fallback: external submission queue
///   4. park_and_wait()
///   For each lease: check demand → poll → dispatch PollResult.

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "celer/core/poll_result.hpp"

namespace celer::async {

// ════════════════════════════════════════════════════════════════════
// StreamLease — The unit of work for the scheduler
// ════════════════════════════════════════════════════════════════════

/// Lightweight token: temporary authority to advance a stream.
/// Workers steal leases, not stream clones.
struct StreamLease {
    void* stream_ctx{nullptr};        // non-owning — scheduler owns the async handle
    const void* vtable{nullptr};      // AsyncStreamVTable<T>* (type-erased)
    StreamBudget budget;
    StreamControl* control{nullptr};  // shared demand/cancel state
    std::uint32_t worker_affinity{0}; // prefer this worker (cache locality)
    std::uint32_t steal_cost{1};      // higher = discourage steal

    [[nodiscard]] auto valid() const noexcept -> bool {
        return stream_ctx != nullptr && vtable != nullptr && control != nullptr;
    }
};

// ════════════════════════════════════════════════════════════════════
// ChaseLevDeque — Lock-free work-stealing deque
// ════════════════════════════════════════════════════════════════════

/// Chase-Lev deque (Dynamic Circular Work-Stealing Deque, 2005).
/// Owner: push_bottom / pop_bottom — single-thread, no CAS on fast path.
/// Thief:  steal — one CAS on top pointer.
///
/// Memory layout: circular buffer with power-of-two capacity.
/// Stores mask (= capacity - 1) instead of capacity — branchless index wrap
/// via bitwise AND. Grows on demand (owner only), never shrinks.
class ChaseLevDeque {
    struct Buffer {
        std::int64_t mask;              // capacity - 1 (power-of-two invariant)
        StreamLease* storage;           // raw pointer: zero indirection on hot path

        explicit Buffer(std::int64_t cap)
            : mask(cap - 1)
            , storage(new StreamLease[static_cast<std::size_t>(cap)]) {}

        ~Buffer() { delete[] storage; }

        Buffer(const Buffer&) = delete;
        auto operator=(const Buffer&) -> Buffer& = delete;

        auto get(std::int64_t idx) noexcept -> StreamLease& {
            return storage[idx & mask];  // branchless power-of-two wrap
        }

        auto grow(std::int64_t top, std::int64_t bottom) -> Buffer* {
            auto new_cap = (mask + 1) * 2;
            auto* nb = new Buffer(new_cap);
            for (auto i = top; i < bottom; ++i) {
                nb->get(i) = std::move(get(i));
            }
            return nb;
        }
    };

    // Cache-line padded to avoid false sharing
    alignas(64) std::atomic<std::int64_t> top_{0};
    alignas(64) std::atomic<std::int64_t> bottom_{0};
    std::atomic<Buffer*> buffer_;

    // Keep old buffers alive until deque destruction (hazard-pointer-free)
    std::vector<Buffer*> old_buffers_;

    static constexpr std::int64_t initial_capacity = 64;

public:
    ChaseLevDeque()
        : buffer_(new Buffer(initial_capacity)) {}

    ~ChaseLevDeque() {
        delete buffer_.load(std::memory_order_relaxed);
        for (auto* b : old_buffers_) delete b;
    }

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    auto operator=(const ChaseLevDeque&) -> ChaseLevDeque& = delete;
    ChaseLevDeque(ChaseLevDeque&&) = delete;
    auto operator=(ChaseLevDeque&&) -> ChaseLevDeque& = delete;

    /// Owner push (LIFO end). Never fails.
    void push(StreamLease lease) noexcept {
        auto b = bottom_.load(std::memory_order_relaxed);
        auto t = top_.load(std::memory_order_acquire);
        auto* buf = buffer_.load(std::memory_order_relaxed);

        if (b - t > buf->mask) [[unlikely]] {
            auto* nb = buf->grow(t, b);
            old_buffers_.push_back(buf);
            buffer_.store(nb, std::memory_order_release);
            buf = nb;
        }

        buf->get(b) = std::move(lease);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    /// Owner pop (LIFO end). Returns nullopt if empty.
    /// Fast path: no CAS when deque has >1 element — [[likely]] annotated.
    [[nodiscard]] auto pop() noexcept -> std::optional<StreamLease> {
        auto b = bottom_.load(std::memory_order_relaxed) - 1;
        auto* buf = buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto t = top_.load(std::memory_order_relaxed);

        auto size = b - t;
        if (size < 0) [[unlikely]] {
            // Empty
            bottom_.store(t, std::memory_order_relaxed);
            return std::nullopt;
        }

        auto lease = std::move(buf->get(b));
        if (size > 0) [[likely]] {
            // >1 element — branchless fast path, no CAS
            return lease;
        }

        // Exactly one element — race with steal
        if (!top_.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
            lease = {};
            bottom_.store(t + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
        bottom_.store(t + 1, std::memory_order_relaxed);
        return lease;
    }

    /// Thief steal (FIFO end). Returns nullopt if empty or contended.
    /// Single CAS — ~8ns under contention.
    [[nodiscard]] auto steal() noexcept -> std::optional<StreamLease> {
        auto t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto b = bottom_.load(std::memory_order_acquire);

        if (t >= b) [[unlikely]] return std::nullopt;

        auto lease = buffer_.load(std::memory_order_relaxed)->get(t);
        if (!top_.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
            return std::nullopt;
        }
        return lease;
    }

    /// Approximate size — branchless max(b - t, 0).
    [[nodiscard]] auto size_approx() const noexcept -> std::int64_t {
        auto b = bottom_.load(std::memory_order_relaxed);
        auto t = top_.load(std::memory_order_relaxed);
        auto diff = b - t;
        // Branchless: arithmetic right-shift sign bit to mask
        return diff & ~(diff >> 63);
    }
};

// ════════════════════════════════════════════════════════════════════
// GlobalQueue — Lock-free MPSC for external schedule() calls
// ════════════════════════════════════════════════════════════════════

/// Simple lock-based global queue. External submitters push here;
/// workers drain it when local deques and steal are empty.
/// Lock-based is acceptable here because global queue access is the
/// cold path — the hot path is local deque push/pop.
class GlobalQueue {
    std::mutex mtx_;
    std::vector<StreamLease> queue_;

public:
    void push(StreamLease lease) {
        std::lock_guard lock(mtx_);
        queue_.push_back(std::move(lease));
    }

    auto pop() -> std::optional<StreamLease> {
        std::lock_guard lock(mtx_);
        if (queue_.empty()) return std::nullopt;
        auto lease = std::move(queue_.back());
        queue_.pop_back();
        return lease;
    }

    [[nodiscard]] auto empty() const -> bool {
        // Intentionally not locked — used for approximation in shutdown draining
        return queue_.empty();
    }
};

// ════════════════════════════════════════════════════════════════════
// ParkingLot — Streams waiting for wake (Pending state)
// ════════════════════════════════════════════════════════════════════

/// Mutex-protected set of parked leases. Parked streams are NOT in any
/// worker's deque — they're waiting for an I/O completion or demand grant
/// to call wake(). This is NOT the hot path.
class ParkingLot {
    std::mutex mtx_;
    std::vector<StreamLease> parked_;

public:
    void park(StreamLease lease) {
        std::lock_guard lock(mtx_);
        parked_.push_back(std::move(lease));
    }

    /// Wake a parked lease by stream context pointer.
    /// Returns the lease or nullopt if not found.
    auto wake(void* stream_ctx) -> std::optional<StreamLease> {
        std::lock_guard lock(mtx_);
        for (auto it = parked_.begin(); it != parked_.end(); ++it) {
            if (it->stream_ctx == stream_ctx) {
                auto lease = std::move(*it);
                parked_.erase(it);
                return lease;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto count() const -> std::size_t {
        std::lock_guard lock(const_cast<std::mutex&>(mtx_));
        return parked_.size();
    }
};

// ════════════════════════════════════════════════════════════════════
// StreamScheduler — Work-stealing pool where work = StreamLease
// ════════════════════════════════════════════════════════════════════

/// Type-erased poll dispatcher. The scheduler doesn't know T, so we
/// store a type-erased function pointer that takes (ctx, vtable, budget, TaskContext&)
/// and returns a type-erased poll result (we use an enum + opaque data).
///
/// For the scheduler's worker loop, we only need the PollKind — the
/// actual Chunk<T> is handled by the poll_fn itself (pushed to output).
using ErasedPollFn = auto(*)(void* ctx, const void* vtable,
                              StreamBudget budget, TaskContext& cx) -> PollKind;

/// Callback invoked when a stream emits a chunk (type-erased).
/// The poll_fn calls this to deliver the chunk to the output queue.
using EmitCallback = void(*)(void* consumer_ctx, void* chunk_ptr);

class StreamScheduler {
public:
    /// Construct with explicit worker count.
    /// Default: hardware_concurrency() (or 2 if detection fails).
    explicit StreamScheduler(std::size_t num_workers = 0)
        : num_workers_(num_workers == 0
            ? std::max(std::size_t{2}, static_cast<std::size_t>(std::thread::hardware_concurrency()))
            : num_workers)
    {
        deques_.reserve(num_workers_);
        for (std::size_t i = 0; i < num_workers_; ++i) {
            deques_.push_back(std::make_unique<ChaseLevDeque>());
        }

        workers_.reserve(num_workers_);
        for (std::size_t i = 0; i < num_workers_; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~StreamScheduler() { shutdown(); }

    StreamScheduler(const StreamScheduler&) = delete;
    auto operator=(const StreamScheduler&) -> StreamScheduler& = delete;
    StreamScheduler(StreamScheduler&&) = delete;
    auto operator=(StreamScheduler&&) -> StreamScheduler& = delete;

    /// Submit a stream lease to the scheduler.
    /// Thread-safe (pushes to global queue).
    void schedule(StreamLease lease) {
        global_queue_.push(std::move(lease));
        notify_one();
    }

    /// Schedule a lease with worker affinity hint.
    /// If affinity is valid, pushes directly to that worker's deque.
    void schedule_affine(StreamLease lease, std::uint32_t worker_id) {
        if (worker_id < num_workers_) {
            deques_[worker_id]->push(std::move(lease));
            notify_one();
        } else {
            schedule(std::move(lease));
        }
    }

    /// Wake a parked stream. Moves it from parking lot to affinity worker's deque.
    void wake(void* stream_ctx) {
        auto lease = parking_lot_.wake(stream_ctx);
        if (lease) {
            schedule_affine(std::move(*lease), lease->worker_affinity);
        }
    }

    /// Number of worker threads.
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t {
        return num_workers_;
    }

    /// Number of parked streams.
    [[nodiscard]] auto parked_count() const -> std::size_t {
        return parking_lot_.count();
    }

    /// Signal workers to drain and exit. Blocks until all joined. Idempotent.
    void shutdown() {
        if (shutdown_.exchange(true, std::memory_order_acq_rel)) return;
        notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    /// Set the type-erased poll function used by the worker loop.
    /// Must be called before scheduling any leases.
    void set_poll_fn(ErasedPollFn fn) noexcept { poll_fn_ = fn; }

private:
    std::size_t num_workers_;

    std::vector<std::unique_ptr<ChaseLevDeque>> deques_;
    GlobalQueue global_queue_;
    ParkingLot parking_lot_;

    std::mutex              wake_mutex_;
    std::condition_variable wake_cv_;

    std::atomic<bool>        shutdown_{false};
    std::vector<std::thread> workers_;
    ErasedPollFn             poll_fn_{nullptr};

    void notify_one() {
        std::lock_guard lock(wake_mutex_);
        wake_cv_.notify_one();
    }

    void notify_all() {
        std::lock_guard lock(wake_mutex_);
        wake_cv_.notify_all();
    }

    void worker_loop(std::size_t id) {
        std::mt19937 rng{static_cast<unsigned>(id * 2654435761u)};
        TaskContext cx{static_cast<std::uint32_t>(id), this};

        while (true) {
            std::optional<StreamLease> opt_lease;

            // 1. Pop local (LIFO — cache-warm)
            opt_lease = deques_[id]->pop();

            // 2. Steal from random peer (FIFO — load balance)
            if (!opt_lease && num_workers_ > 1) {
                auto victim_dist = std::uniform_int_distribution<std::size_t>(0, num_workers_ - 2);
                auto victim = victim_dist(rng);
                if (victim >= id) ++victim;
                opt_lease = deques_[victim]->steal();
            }

            // 3. Pop from global submission queue
            if (!opt_lease) {
                opt_lease = global_queue_.pop();
            }

            if (opt_lease) {
                auto& lease = *opt_lease;
                lease.worker_affinity = static_cast<std::uint32_t>(id);

                // Check cancellation
                if (lease.control && lease.control->cancelled.load(std::memory_order_acquire)) {
                    continue;  // reap — drop the lease
                }

                // Check demand
                if (lease.control && !lease.control->should_advance()) {
                    parking_lot_.park(std::move(lease));
                    continue;
                }

                // Poll the stream
                if (poll_fn_) {
                    auto kind = poll_fn_(lease.stream_ctx, lease.vtable, lease.budget, cx);
                    dispatch_result(kind, std::move(lease), id);
                }
                continue;
            }

            // 4. No work — park and wait, or exit if shutting down
            if (shutdown_.load(std::memory_order_acquire)) {
                drain_remaining(id);
                return;
            }

            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::microseconds(50), [this] {
                return shutdown_.load(std::memory_order_acquire)
                    || !global_queue_.empty();
            });
        }
    }

    void dispatch_result(PollKind kind, StreamLease lease, std::size_t worker_id) {
        switch (kind) {
            case PollKind::Emit:
                // Chunk was already delivered via the poll_fn's emit callback.
                // Requeue locally for more work.
                if (lease.control) lease.control->on_emit();
                deques_[worker_id]->push(std::move(lease));
                break;
            case PollKind::Pending:
                // I/O wait — park until wake().
                parking_lot_.park(std::move(lease));
                break;
            case PollKind::Yield:
                // Budget exhausted — requeue locally (may be stolen).
                deques_[worker_id]->push(std::move(lease));
                break;
            case PollKind::Done:
                // Stream exhausted — lease dropped.
                break;
            case PollKind::Error:
                // Error — lease dropped. Error was reported via poll_fn.
                break;
        }
    }

    void drain_remaining(std::size_t id) {
        // Drain local deque
        while (auto lease = deques_[id]->pop()) {
            if (poll_fn_ && lease->valid()) {
                TaskContext cx{static_cast<std::uint32_t>(id), this};
                poll_fn_(lease->stream_ctx, lease->vtable, lease->budget, cx);
            }
        }
    }
};

// ── TaskContext::wake implementation (needs StreamScheduler definition) ──

inline void TaskContext::wake(StreamLease& lease) const {
    if (scheduler) {
        scheduler->wake(lease.stream_ctx);
    }
}

} // namespace celer::async

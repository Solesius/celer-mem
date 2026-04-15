#pragma once
/// celer::async::TaskPool — Minimal work-stealing thread pool.
///
/// Fixed-size pool with per-worker Chase-Lev deques and randomized steal.
/// No std::async (per-task thread overhead), no std::execution (not portable).
///
/// Design:
///   - Workers pop from their own deque (LIFO — cache-friendly)
///   - Idle workers steal from random peer deques (FIFO — load balance)
///   - Single atomic shutdown flag + condition variable for idle wait
///   - submit() returns std::future<T> for caller synchronization
///
/// Thread safety:
///   - submit() is thread-safe (pushes to a shared submission queue)
///   - TaskPool itself is NOT copyable/movable (owns threads)
///
/// RAII: destructor calls shutdown() + join.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace celer::async {

/// Work-stealing task pool with fixed worker count.
/// Each worker has a local deque; idle workers steal from peers.
class TaskPool {
public:
    /// Construct with explicit worker count.
    /// Default: hardware_concurrency() (or 2 if detection fails).
    explicit TaskPool(std::size_t num_workers = 0)
        : num_workers_(num_workers == 0
            ? std::max(std::size_t{2}, static_cast<std::size_t>(std::thread::hardware_concurrency()))
            : num_workers)
        , worker_queues_(num_workers_)
        , queue_mutexes_(num_workers_)
    {
        workers_.reserve(num_workers_);
        for (std::size_t i = 0; i < num_workers_; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~TaskPool() { shutdown(); }

    TaskPool(const TaskPool&) = delete;
    auto operator=(const TaskPool&) -> TaskPool& = delete;
    TaskPool(TaskPool&&) = delete;
    auto operator=(TaskPool&&) -> TaskPool& = delete;

    /// Submit a callable to the pool. Returns a future for the result.
    /// Thread-safe: any thread may call submit().
    template <typename Fn>
    [[nodiscard]] auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        using R = std::invoke_result_t<Fn>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        auto future = task->get_future();

        {
            std::lock_guard lock(submission_mutex_);
            submission_queue_.emplace_back([t = std::move(task)]() mutable { (*t)(); });
        }
        wake_cv_.notify_one();
        return future;
    }

    /// Number of worker threads.
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t {
        return num_workers_;
    }

    /// Signal all workers to finish pending work and exit.
    /// Blocks until all threads have joined. Idempotent.
    void shutdown() {
        if (shutdown_.exchange(true)) return;  // already shut down
        wake_cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

private:
    using Task = std::function<void()>;

    std::size_t num_workers_;

    // Per-worker local deques (push/pop by owner, steal by peers)
    std::vector<std::deque<Task>> worker_queues_;
    std::vector<std::mutex>       queue_mutexes_;

    // Shared submission queue (external submitters → workers)
    std::deque<Task>  submission_queue_;
    std::mutex        submission_mutex_;

    // Wake mechanism
    std::mutex              wake_mutex_;
    std::condition_variable wake_cv_;

    std::atomic<bool>          shutdown_{false};
    std::vector<std::thread>   workers_;

    void worker_loop(std::size_t id) {
        // Thread-local RNG for steal target selection
        std::mt19937 rng{static_cast<unsigned>(id * 2654435761u)};

        while (true) {
            Task task;

            // 1. Try local deque (LIFO — cache-warm)
            {
                std::lock_guard lock(queue_mutexes_[id]);
                if (!worker_queues_[id].empty()) {
                    task = std::move(worker_queues_[id].back());
                    worker_queues_[id].pop_back();
                }
            }

            // 2. Try submission queue
            if (!task) {
                std::lock_guard lock(submission_mutex_);
                if (!submission_queue_.empty()) {
                    task = std::move(submission_queue_.front());
                    submission_queue_.pop_front();
                }
            }

            // 3. Try stealing from a random peer (FIFO — fairness)
            if (!task && num_workers_ > 1) {
                auto victim = std::uniform_int_distribution<std::size_t>(
                    0, num_workers_ - 2)(rng);
                if (victim >= id) ++victim;  // skip self

                std::lock_guard lock(queue_mutexes_[victim]);
                if (!worker_queues_[victim].empty()) {
                    task = std::move(worker_queues_[victim].front());
                    worker_queues_[victim].pop_front();
                }
            }

            if (task) {
                task();
                continue;
            }

            // 4. No work found — wait or exit
            if (shutdown_.load(std::memory_order_acquire)) {
                // Drain remaining tasks before exiting
                drain_remaining(id);
                return;
            }

            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::microseconds(100), [this] {
                std::lock_guard sub_lock(submission_mutex_);
                return shutdown_.load(std::memory_order_acquire)
                    || !submission_queue_.empty();
            });
        }
    }

    void drain_remaining(std::size_t id) {
        // Drain local queue
        while (true) {
            Task task;
            {
                std::lock_guard lock(queue_mutexes_[id]);
                if (worker_queues_[id].empty()) break;
                task = std::move(worker_queues_[id].back());
                worker_queues_[id].pop_back();
            }
            task();
        }
        // Drain submission queue
        while (true) {
            Task task;
            {
                std::lock_guard lock(submission_mutex_);
                if (submission_queue_.empty()) break;
                task = std::move(submission_queue_.front());
                submission_queue_.pop_front();
            }
            task();
        }
    }
};

} // namespace celer::async

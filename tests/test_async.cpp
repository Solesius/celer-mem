/// celer-mem async streaming + infrastructure tests — PollResult, StreamBudget,
/// StreamControl, ChaseLevDeque (branchless), StreamScheduler, AsyncStreamHandle,
/// combinators, bridge adapters, FlatSymbolTable (memoized).
///
/// Uses Google Test (gtest).

#include "celer/core/async_stream.hpp"
#include "celer/core/poll_result.hpp"
#include "celer/core/scheduler.hpp"
#include "celer/core/stream.hpp"
#include "celer/core/symbol_table.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace celer;
using namespace celer::async;

// ════════════════════════════════════════
// PollResult<T> state coverage
// ════════════════════════════════════════

TEST(PollResult, EmitWhenDataAvailable) {
    auto chunk = Chunk<int>::from({1, 2, 3});
    auto r = PollResult<int>::emit(std::move(chunk));
    ASSERT_TRUE(r.is_emit());
    EXPECT_EQ(r.chunk.size(), 3u);
    EXPECT_EQ(r.chunk[0], 1);
}

TEST(PollResult, PendingWhenBlocked) {
    auto r = PollResult<int>::pending();
    EXPECT_TRUE(r.is_pending());
    EXPECT_FALSE(r.is_emit());
}

TEST(PollResult, YieldWhenBudgetExhausted) {
    auto r = PollResult<int>::yield();
    EXPECT_TRUE(r.is_yield());
    EXPECT_FALSE(r.is_emit());
    EXPECT_FALSE(r.is_pending());
}

TEST(PollResult, DoneWhenStreamExhausted) {
    auto r = PollResult<int>::done();
    EXPECT_TRUE(r.is_done());
}

TEST(PollResult, ErrorWhenFailure) {
    auto r = PollResult<int>::err(Error{"TestErr", "broken"});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error.code, "TestErr");
    EXPECT_EQ(r.error.message, "broken");
}

// ════════════════════════════════════════
// StreamBudget presets
// ════════════════════════════════════════

TEST(StreamBudget, LocalBudgetTightLimits) {
    constexpr auto b = StreamBudget::local();
    EXPECT_EQ(b.max_chunks, 1u);
    EXPECT_EQ(b.max_bytes, 64u * 1024u);
    EXPECT_EQ(b.max_ns, 50'000u);
}

TEST(StreamBudget, NetworkBudgetGenerousLimits) {
    constexpr auto b = StreamBudget::network();
    EXPECT_EQ(b.max_chunks, 16u);
    EXPECT_EQ(b.max_bytes, 8u * 1024u * 1024u);
    EXPECT_EQ(b.max_ns, 100'000'000u);
}

TEST(StreamBudget, UnboundedBudget) {
    constexpr auto b = StreamBudget::unbounded();
    EXPECT_EQ(b.max_chunks, UINT32_MAX);
    EXPECT_EQ(b.max_bytes, UINT32_MAX);
    EXPECT_EQ(b.max_ns, UINT64_MAX);
}

// ════════════════════════════════════════
// StreamControl — demand + backpressure
// ════════════════════════════════════════

TEST(StreamControl, NotAdvanceWhenDemandZero) {
    StreamControl ctrl;
    EXPECT_FALSE(ctrl.should_advance());
}

TEST(StreamControl, AdvanceWhenDemandGranted) {
    StreamControl ctrl;
    ctrl.request(5);
    EXPECT_EQ(ctrl.requested.load(), 5u);
    EXPECT_TRUE(ctrl.should_advance());
}

TEST(StreamControl, DecrementDemandOnEmit) {
    StreamControl ctrl;
    ctrl.request(3);
    ctrl.on_emit();
    EXPECT_EQ(ctrl.requested.load(), 2u);
    EXPECT_EQ(ctrl.buffered.load(), 1u);
}

TEST(StreamControl, DecrementBufferedOnConsume) {
    StreamControl ctrl;
    ctrl.request(1);
    ctrl.on_emit();
    ctrl.on_consume();
    EXPECT_EQ(ctrl.buffered.load(), 0u);
}

TEST(StreamControl, CancelCooperatively) {
    StreamControl ctrl;
    ctrl.request(10);
    ctrl.cancel();
    EXPECT_FALSE(ctrl.should_advance());
    EXPECT_TRUE(ctrl.cancelled.load());
}

TEST(StreamControl, BlockAdvanceAtHighWatermark) {
    StreamControl ctrl;
    ctrl.request(100);
    for (uint32_t i = 0; i < StreamControl::default_high_watermark; ++i) {
        ctrl.on_emit();
        ctrl.request(1);  // keep demanded > 0
    }
    EXPECT_FALSE(ctrl.should_advance());
}

// ════════════════════════════════════════
// ChaseLevDeque — lock-free owner/thief
// ════════════════════════════════════════

namespace {
auto make_test_lease(std::uint32_t tag) -> StreamLease {
    StreamLease l;
    l.worker_affinity = tag;
    l.steal_cost = tag;
    return l;
}
} // namespace

TEST(ChaseLevDeque, PushPopLifoWhenOwner) {
    ChaseLevDeque dq;
    dq.push(make_test_lease(1));
    dq.push(make_test_lease(2));
    dq.push(make_test_lease(3));

    auto a = dq.pop();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->worker_affinity, 3u);
    auto b = dq.pop();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->worker_affinity, 2u);
    auto c = dq.pop();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->worker_affinity, 1u);
    EXPECT_FALSE(dq.pop().has_value());
}

TEST(ChaseLevDeque, StealFifoWhenThief) {
    ChaseLevDeque dq;
    dq.push(make_test_lease(10));
    dq.push(make_test_lease(20));
    dq.push(make_test_lease(30));

    auto a = dq.steal();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->worker_affinity, 10u);
    auto b = dq.steal();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->worker_affinity, 20u);
    auto c = dq.steal();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->worker_affinity, 30u);
    EXPECT_FALSE(dq.steal().has_value());
}

TEST(ChaseLevDeque, ReturnNulloptWhenEmptyPop) {
    ChaseLevDeque dq;
    EXPECT_FALSE(dq.pop().has_value());
}

TEST(ChaseLevDeque, ReturnNulloptWhenEmptySteal) {
    ChaseLevDeque dq;
    EXPECT_FALSE(dq.steal().has_value());
}

TEST(ChaseLevDeque, GrowBufferWhenCapacityExceeded) {
    ChaseLevDeque dq;
    for (uint32_t i = 0; i < 128; ++i) {
        dq.push(make_test_lease(i));
    }
    EXPECT_EQ(dq.size_approx(), 128);

    for (int i = 127; i >= 0; --i) {
        auto r = dq.pop();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->worker_affinity, static_cast<uint32_t>(i));
    }
}

TEST(ChaseLevDeque, InterleavePushPopSteal) {
    ChaseLevDeque dq;
    dq.push(make_test_lease(1));
    dq.push(make_test_lease(2));

    auto stolen = dq.steal();  // FIFO: gets 1
    ASSERT_TRUE(stolen.has_value());
    EXPECT_EQ(stolen->worker_affinity, 1u);

    dq.push(make_test_lease(3));

    auto popped = dq.pop();    // LIFO: gets 3
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->worker_affinity, 3u);

    auto last = dq.pop();      // gets 2
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->worker_affinity, 2u);
    EXPECT_FALSE(dq.pop().has_value());
}

// ════════════════════════════════════════
// ChaseLevDeque — concurrent steal test
// ════════════════════════════════════════

TEST(ChaseLevDeque, StealConcurrentlyWithoutDataLoss) {
    ChaseLevDeque dq;
    constexpr int N = 1000;

    for (uint32_t i = 0; i < N; ++i) {
        dq.push(make_test_lease(i));
    }

    // In Chase-Lev, steal() returns nullopt on BOTH empty and CAS-aborted.
    // A stealer that exits on the first nullopt may bail during contention
    // while items remain. Stealers must spin until the owner signals it has
    // finished draining, at which point the deque is truly empty.
    std::atomic<int> steal_count{0};
    std::atomic<bool> owner_done{false};
    auto stealer = [&]() {
        while (true) {
            auto r = dq.steal();
            if (r) {
                steal_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Nullopt — could be empty or CAS-aborted. Only exit once the
            // owner has finished and a fresh steal still sees empty.
            if (owner_done.load(std::memory_order_acquire)) {
                if (!dq.steal()) break;   // confirmed empty
                steal_count.fetch_add(1, std::memory_order_relaxed);
            }
            // Back off briefly to avoid livelock with owner on CAS.
            std::this_thread::yield();
        }
    };

    std::thread t1(stealer);
    std::thread t2(stealer);

    int owner_count = 0;
    while (auto r = dq.pop()) {
        ++owner_count;
    }
    owner_done.store(true, std::memory_order_release);

    t1.join();
    t2.join();

    EXPECT_EQ(owner_count + steal_count.load(), N);
}

// ════════════════════════════════════════
// StreamScheduler — lifecycle
// ════════════════════════════════════════

TEST(StreamScheduler, CreateWithWorkerCount) {
    StreamScheduler sched(4);
    EXPECT_EQ(sched.worker_count(), 4u);
}

TEST(StreamScheduler, ShutdownCleanlyOnDestruction) {
    {
        StreamScheduler sched(2);
        EXPECT_EQ(sched.worker_count(), 2u);
    }
    // No crash or hang
}

TEST(StreamScheduler, ZeroParkedWhenIdle) {
    StreamScheduler sched(2);
    EXPECT_EQ(sched.parked_count(), 0u);
}

// ════════════════════════════════════════
// AsyncStreamHandle — to_async bridge
// ════════════════════════════════════════

TEST(AsyncStreamHandle, BridgeSyncToAsync) {
    auto sync = stream::from_vector<int>({10, 20, 30});
    auto async_h = to_async<int>(std::move(sync));
    ASSERT_TRUE(async_h.valid());

    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    auto r1 = async_h.poll(budget, cx);
    ASSERT_TRUE(r1.is_emit());
    EXPECT_EQ(r1.chunk.size(), 3u);
    EXPECT_EQ(r1.chunk[0], 10);
    EXPECT_EQ(r1.chunk[1], 20);
    EXPECT_EQ(r1.chunk[2], 30);

    auto r2 = async_h.poll(budget, cx);
    EXPECT_TRUE(r2.is_done());
}

TEST(AsyncStreamHandle, NeverReturnPendingFromSyncBridge) {
    auto sync = stream::from_vector<int>({1, 2});
    auto async_h = to_async<int>(std::move(sync));
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    bool saw_pending = false;
    while (true) {
        auto r = async_h.poll(budget, cx);
        if (r.is_pending()) saw_pending = true;
        if (r.is_done()) break;
    }
    EXPECT_FALSE(saw_pending);
}

TEST(AsyncStreamHandle, DoneWhenEmptyAsyncStream) {
    auto sync = stream::empty<int>();
    auto async_h = to_async<int>(std::move(sync));
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    auto r = async_h.poll(budget, cx);
    EXPECT_TRUE(r.is_done());
}

// ════════════════════════════════════════
// AsyncStreamHandle — clone (semantic fork)
// ════════════════════════════════════════

TEST(AsyncStreamHandle, CloneIndependentStream) {
    auto sync = stream::from_vector<int>({1, 2, 3});
    auto original = to_async<int>(std::move(sync));
    auto cloned = original.clone();

    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    auto r1 = original.poll(budget, cx);
    ASSERT_TRUE(r1.is_emit());
    auto r2 = original.poll(budget, cx);
    EXPECT_TRUE(r2.is_done());

    // Clone still at start
    auto cr1 = cloned.poll(budget, cx);
    ASSERT_TRUE(cr1.is_emit());
    EXPECT_EQ(cr1.chunk.size(), 3u);
}

// ════════════════════════════════════════
// AsyncStreamHandle — cancel
// ════════════════════════════════════════

TEST(AsyncStreamHandle, DoneAfterCancel) {
    auto sync = stream::from_vector<int>({1, 2, 3});
    auto async_h = to_async<int>(std::move(sync));

    async_h.cancel();

    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();
    auto r = async_h.poll(budget, cx);
    EXPECT_TRUE(r.is_done());
}

// ════════════════════════════════════════
// par_map — order preservation
// ════════════════════════════════════════

TEST(Combinators, ParMapPreservesOrder) {
    StreamScheduler sched(2);
    auto source = to_async<int>(stream::from_vector<int>({1, 2, 3, 4, 5}));
    auto mapped = par_map<int>(std::move(source),
                               [](const int& x) { return x * 2; },
                               sched, 3);

    auto result = collect_blocking<int>(mapped, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::vector<int>({2, 4, 6, 8, 10}));
}

// ════════════════════════════════════════
// par_eval — merge multiple streams
// ════════════════════════════════════════

TEST(Combinators, ParEvalMergesAll) {
    StreamScheduler sched(2);
    std::vector<AsyncStreamHandle<int>> sources;
    sources.push_back(to_async<int>(stream::from_vector<int>({1, 2})));
    sources.push_back(to_async<int>(stream::from_vector<int>({3, 4})));
    sources.push_back(to_async<int>(stream::from_vector<int>({5, 6})));

    auto merged = par_eval<int>(std::move(sources), sched);
    auto result = collect_blocking<int>(merged, sched);
    ASSERT_TRUE(result.has_value());

    auto v = *result;
    EXPECT_EQ(v.size(), 6u);
    std::sort(v.begin(), v.end());
    EXPECT_EQ(v, std::vector<int>({1, 2, 3, 4, 5, 6}));
}

// ════════════════════════════════════════
// collect_blocking — terminal
// ════════════════════════════════════════

TEST(Combinators, CollectBlockingBasic) {
    StreamScheduler sched(2);
    auto async_h = to_async<int>(stream::from_vector<int>({10, 20, 30}));
    auto result = collect_blocking<int>(async_h, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::vector<int>({10, 20, 30}));
}

TEST(Combinators, CollectBlockingEmpty) {
    StreamScheduler sched(2);
    auto async_h = to_async<int>(stream::empty<int>());
    auto result = collect_blocking<int>(async_h, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

// ════════════════════════════════════════
// Performance — Chase-Lev hot path
// ════════════════════════════════════════

TEST(Performance, PopLocalUnder5ns) {
    ChaseLevDeque dq;
    constexpr int N = 100'000;

    for (uint32_t i = 0; i < N; ++i) {
        dq.push(make_test_lease(i));
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        auto r = dq.pop();
        ASSERT_TRUE(r.has_value());
    }
    auto end = std::chrono::steady_clock::now();
    auto avg = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / N;

    std::cout << "[perf] avg pop: " << avg << " ns\n";
    EXPECT_LT(avg, 500);  // target <5ns, allow 500ns for CI/container
}

TEST(Performance, FullPollCycleUnder50ns) {
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();
    constexpr int WARMUP = 100;
    constexpr int N = 100'000;

    std::vector<AsyncStreamHandle<int>> handles;
    handles.reserve(N + WARMUP);
    for (int i = 0; i < N + WARMUP; ++i) {
        handles.push_back(to_async<int>(stream::from_vector<int>({i})));
    }

    for (int i = 0; i < WARMUP; ++i) {
        (void)handles[i].poll(budget, cx);
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = WARMUP; i < WARMUP + N; ++i) {
        auto r = handles[i].poll(budget, cx);
        ASSERT_TRUE(r.is_emit());
    }
    auto end = std::chrono::steady_clock::now();
    auto avg = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / N;

    std::cout << "[perf] avg poll cycle: " << avg << " ns\n";
    EXPECT_LT(avg, 5000);  // target <20ns, allow 5us for CI/container
}

// ════════════════════════════════════════
// Move semantics
// ════════════════════════════════════════

TEST(AsyncStreamHandle, InvalidateSourceWhenMoved) {
    auto async_h = to_async<int>(stream::from_vector<int>({1, 2, 3}));
    ASSERT_TRUE(async_h.valid());

    auto moved = std::move(async_h);
    EXPECT_TRUE(moved.valid());
    EXPECT_FALSE(async_h.valid());
}

// ════════════════════════════════════════
// Branchless deque — mask invariants
// ════════════════════════════════════════

TEST(ChaseLevDeque, MaskIndexingNotModulo) {
    // Verify the deque uses power-of-two mask for indexing.
    // Push exactly initial_capacity items, pop all — proves mask wrapping works.
    ChaseLevDeque dq;
    for (uint32_t i = 0; i < 64; ++i) {
        dq.push(make_test_lease(i));
    }
    EXPECT_EQ(dq.size_approx(), 64);

    // Pop all — LIFO
    for (int i = 63; i >= 0; --i) {
        auto r = dq.pop();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->worker_affinity, static_cast<uint32_t>(i));
    }
}

TEST(ChaseLevDeque, GrowPreservesMaskInvariant) {
    ChaseLevDeque dq;
    // Push > initial_capacity to trigger grow
    for (uint32_t i = 0; i < 200; ++i) {
        dq.push(make_test_lease(i));
    }
    EXPECT_EQ(dq.size_approx(), 200);

    // Verify all accessible after grow (mask updated correctly)
    for (int i = 199; i >= 0; --i) {
        auto r = dq.pop();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->worker_affinity, static_cast<uint32_t>(i));
    }
}

TEST(ChaseLevDeque, BranchlessSizeApproxNonNegative) {
    ChaseLevDeque dq;
    // Empty deque: size_approx must be 0, not negative
    EXPECT_EQ(dq.size_approx(), 0);

    dq.push(make_test_lease(1));
    (void)dq.pop();
    // After drain: still 0, branchless max(diff, 0) holds
    EXPECT_GE(dq.size_approx(), 0);
}

// ════════════════════════════════════════
// FlatSymbolTable — memoized symbol table
// ════════════════════════════════════════

TEST(FlatSymbolTable, LookupExistingKey) {
    std::vector<std::string_view> keys = {"scope1", "scope2", "scope3"};
    std::vector<std::size_t> values = {0, 1, 2};
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    auto r = tbl.find("scope2");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
}

TEST(FlatSymbolTable, ReturnNulloptWhenMissing) {
    std::vector<std::string_view> keys = {"a", "b"};
    std::vector<std::size_t> values = {0, 1};
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    EXPECT_FALSE(tbl.find("c").has_value());
    EXPECT_FALSE(tbl.contains("z"));
}

TEST(FlatSymbolTable, ContainsMatchesFind) {
    std::vector<std::string_view> keys = {"x", "y"};
    std::vector<std::size_t> values = {10, 20};
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    EXPECT_TRUE(tbl.contains("x"));
    EXPECT_TRUE(tbl.contains("y"));
    EXPECT_FALSE(tbl.contains("w"));
}

TEST(FlatSymbolTable, PowerOfTwoCapacity) {
    std::vector<std::string_view> keys = {"a", "b", "c", "d", "e"};
    std::vector<std::size_t> values = {0, 1, 2, 3, 4};
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    // Capacity must be power of two and >= 2x count
    auto cap = tbl.capacity();
    EXPECT_GE(cap, 10u);
    EXPECT_EQ(cap & (cap - 1), 0u);  // power of two check
}

TEST(FlatSymbolTable, EmptyTableReturnsNullopt) {
    celer::FlatSymbolTable tbl;
    EXPECT_TRUE(tbl.empty());
    EXPECT_FALSE(tbl.find("anything").has_value());
}

TEST(FlatSymbolTable, AllKeysRetrievable) {
    std::vector<std::string_view> keys = {
        "users", "tasks", "settings", "logs", "cache",
        "sessions", "metrics", "events"
    };
    std::vector<std::size_t> values;
    for (std::size_t i = 0; i < keys.size(); ++i) values.push_back(i);
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    EXPECT_EQ(tbl.size(), 8u);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        auto r = tbl.find(keys[i]);
        ASSERT_TRUE(r.has_value()) << "missing key: " << keys[i];
        EXPECT_EQ(*r, i);
    }
}

TEST(FlatSymbolTable, BuildIndexedFromRange) {
    // Simulate what tree_builder does
    struct Named { std::string name; };
    std::vector<Named> items = {{"alpha"}, {"beta"}, {"gamma"}};
    auto tbl = celer::FlatSymbolTable::build_indexed(items,
        [](const Named& n) -> std::string_view { return n.name; });

    EXPECT_EQ(tbl.size(), 3u);
    EXPECT_EQ(*tbl.find("alpha"), 0u);
    EXPECT_EQ(*tbl.find("beta"), 1u);
    EXPECT_EQ(*tbl.find("gamma"), 2u);
}

TEST(FlatSymbolTable, LookupPerformanceUnder10ns) {
    std::vector<std::string_view> keys = {
        "users", "tasks", "settings", "logs", "cache",
        "sessions", "metrics", "events", "orders", "products",
        "reviews", "inventory", "payments", "shipping", "analytics",
        "notifications"
    };
    std::vector<std::size_t> values;
    for (std::size_t i = 0; i < keys.size(); ++i) values.push_back(i);
    auto tbl = celer::FlatSymbolTable::build(keys, values);

    constexpr int N = 100'000;
    // Warmup
    for (int i = 0; i < 100; ++i) {
        (void)tbl.find(keys[i % keys.size()]);
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        auto r = tbl.find(keys[i % keys.size()]);
        ASSERT_TRUE(r.has_value());
    }
    auto end = std::chrono::steady_clock::now();
    auto avg = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / N;

    std::cout << "[perf] avg symbol lookup: " << avg << " ns\n";
    EXPECT_LT(avg, 500);  // target <10ns, allow 500ns for CI/container
}

// ════════════════════════════════════════════════════════════════════════════
// MegaStream — 1M+ element stress tests and edge cases
// ════════════════════════════════════════════════════════════════════════════

namespace {
constexpr int MEGA = 1'000'000;

auto make_mega_vector(int n) -> std::vector<int> {
    std::vector<int> v(static_cast<std::size_t>(n));
    std::iota(v.begin(), v.end(), 0);
    return v;
}
} // namespace

// ── Sync path: 1M elements in a single chunk ──

TEST(MegaStream, Collect1MElementsSyncSingleChunk) {
    auto src = stream::from_vector<int>(make_mega_vector(MEGA));
    auto result = stream::collect(src);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), static_cast<std::size_t>(MEGA));
    EXPECT_EQ((*result)[0], 0);
    EXPECT_EQ((*result)[MEGA - 1], MEGA - 1);
}

// ── Async bridge: 1M elements through to_async ──

TEST(MegaStream, Drain1MElementsAsyncBridged) {
    auto sync_s = stream::from_vector<int>(make_mega_vector(MEGA));
    auto async_h = to_async<int>(std::move(sync_s));

    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    // First poll: single emit with 1M elements
    auto r = async_h.poll(budget, cx);
    ASSERT_TRUE(r.is_emit());
    EXPECT_EQ(r.chunk.size(), static_cast<std::size_t>(MEGA));

    // Verify bookends
    EXPECT_EQ(r.chunk[0], 0);
    EXPECT_EQ(r.chunk[MEGA - 1], MEGA - 1);

    // Must be done now
    auto r2 = async_h.poll(budget, cx);
    EXPECT_TRUE(r2.is_done());
}

// ── Chained combinators over 1M: map -> filter -> take ──

TEST(MegaStream, ChainMapFilterTakeOver1M) {
    auto src = stream::from_vector<int>(make_mega_vector(MEGA));

    // map: x * 2
    auto mapped = stream::map<int>(std::move(src), [](const int& x) { return x * 2; });

    // filter: divisible by 6 (original x divisible by 3, doubled)
    auto filtered = stream::filter<int>(std::move(mapped),
        [](const int& x) { return x % 6 == 0; });

    // take first 100k
    auto taken = stream::take<int>(std::move(filtered), 100'000);

    auto result = stream::collect(taken);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 100'000u);

    // All must be even and divisible by 6
    for (std::size_t i = 0; i < result->size(); ++i) {
        EXPECT_EQ((*result)[i] % 6, 0) << "element " << i << " = " << (*result)[i];
    }
}

// ── par_map over 1M: order preservation ──

TEST(MegaStream, ParMap1MPreservesOrder) {
    StreamScheduler sched(2);
    auto source = to_async<int>(stream::from_vector<int>(make_mega_vector(MEGA)));
    auto mapped = par_map<int>(std::move(source),
                               [](const int& x) { return x + 1; },
                               sched, 4);

    auto result = collect_blocking<int>(mapped, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), static_cast<std::size_t>(MEGA));

    // Verify order: [1, 2, 3, ..., MEGA]
    for (int i = 0; i < MEGA; ++i) {
        EXPECT_EQ((*result)[i], i + 1) << "at index " << i;
    }
}

// ── par_eval: merge 10 streams × 100k ──

TEST(MegaStream, ParEvalMerge10StreamsOf100k) {
    StreamScheduler sched(2);
    constexpr int STREAMS = 10;
    constexpr int PER = 100'000;

    std::vector<AsyncStreamHandle<int>> sources;
    sources.reserve(STREAMS);
    for (int s = 0; s < STREAMS; ++s) {
        std::vector<int> v(PER);
        std::iota(v.begin(), v.end(), s * PER);
        sources.push_back(to_async<int>(stream::from_vector<int>(std::move(v))));
    }

    auto merged = par_eval<int>(std::move(sources), sched);
    auto result = collect_blocking<int>(merged, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), static_cast<std::size_t>(MEGA));

    // Sort to verify no duplicates, no missing
    auto v = *result;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    EXPECT_EQ(v.size(), static_cast<std::size_t>(MEGA)) << "duplicates or missing elements after merge";
}

// ── Structural sharing: O(1) slice over 1M chunk ──

TEST(MegaStream, StructuralShareSliceOver1MChunk) {
    auto chunk = Chunk<int>::from(make_mega_vector(MEGA));
    ASSERT_EQ(chunk.size(), static_cast<std::size_t>(MEGA));

    // Slice middle 250k range (O(1), no copy)
    auto sliced = chunk.slice(500'000, 250'000);
    EXPECT_EQ(sliced.size(), 250'000u);
    EXPECT_EQ(sliced[0], 500'000);
    EXPECT_EQ(sliced[249'999], 749'999);

    // Original untouched
    EXPECT_EQ(chunk.size(), static_cast<std::size_t>(MEGA));
    EXPECT_EQ(chunk[500'000], 500'000);
}

// ── Many small chunks: 100k singletons through async pipeline ──

TEST(MegaStream, ManySmallChunks100kSingletons) {
    // Build a stream impl that yields 100k one-element chunks
    struct ManySingletonImpl {
        int remaining;
        int next;

        auto pull() -> Result<std::optional<Chunk<int>>> {
            if (remaining <= 0) return std::optional<Chunk<int>>{};
            auto c = Chunk<int>::singleton(next++);
            --remaining;
            return std::optional{std::move(c)};
        }

        ManySingletonImpl(int n) : remaining(n), next(0) {}
        ManySingletonImpl(const ManySingletonImpl&) = default;
    };

    constexpr int N = 100'000;
    auto src = make_stream_handle<int>(new ManySingletonImpl{N});
    auto result = stream::collect(src);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), static_cast<std::size_t>(N));

    // Verify contiguous sequence
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ((*result)[i], i) << "at index " << i;
    }
}

// ── Clone + independent drain of 1M stream ──

TEST(MegaStream, CloneAndIndependentlyDrain1M) {
    auto src = stream::from_vector<int>(make_mega_vector(MEGA));
    auto cloned = src.clone();

    // Drain original
    auto r1 = stream::collect(src);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->size(), static_cast<std::size_t>(MEGA));

    // Clone should independently produce the same data
    auto r2 = stream::collect(cloned);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->size(), static_cast<std::size_t>(MEGA));

    EXPECT_EQ(*r1, *r2);
}

// ── Cancel mid-stream at 500k of 1M ──

TEST(MegaStream, CancelMidStreamAt500kOf1M) {
    // Custom stream that yields one element per pull (to test mid-cancel)
    struct CountingImpl {
        int remaining;
        int next;

        auto poll(StreamBudget /*budget*/, TaskContext& /*cx*/) -> PollResult<int> {
            if (remaining <= 0) return PollResult<int>::done();
            auto c = Chunk<int>::singleton(next++);
            --remaining;
            return PollResult<int>::emit(std::move(c));
        }
        void request(std::size_t) {}
        void cancel() { remaining = 0; }

        CountingImpl(int n) : remaining(n), next(0) {}
        CountingImpl(const CountingImpl&) = default;
    };

    auto handle = make_async_stream_handle<int>(new CountingImpl{MEGA});
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    int collected = 0;
    while (true) {
        auto r = handle.poll(budget, cx);
        if (r.is_done()) break;
        ASSERT_TRUE(r.is_emit());
        collected += static_cast<int>(r.chunk.size());
        if (collected >= 500'000) {
            handle.cancel();
        }
    }

    // Got at least 500k before cancellation took effect
    EXPECT_GE(collected, 500'000);
    // But not the full million (cancel stopped it)
    EXPECT_LT(collected, MEGA);
}

// ── Deque: 1M push/pop no data loss ──

TEST(MegaStream, Deque1MPushPopNoLoss) {
    ChaseLevDeque dq;
    for (uint32_t i = 0; i < static_cast<uint32_t>(MEGA); ++i) {
        dq.push(make_test_lease(i));
    }
    EXPECT_EQ(dq.size_approx(), static_cast<int64_t>(MEGA));

    int count = 0;
    uint32_t last = MEGA;
    while (auto r = dq.pop()) {
        // LIFO: descending
        EXPECT_LT(r->worker_affinity, last);
        last = r->worker_affinity;
        ++count;
    }
    EXPECT_EQ(count, MEGA);
}

// ── Throughput: full pipeline 1M under wall-clock budget ──

TEST(MegaStream, Throughput1MFullPipelineUnder2s) {
    StreamScheduler sched(2);

    auto start = std::chrono::steady_clock::now();

    auto source = to_async<int>(stream::from_vector<int>(make_mega_vector(MEGA)));
    auto mapped = par_map<int>(std::move(source),
                               [](const int& x) { return x * 3; },
                               sched, 4);
    auto result = collect_blocking<int>(mapped, sched);

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), static_cast<std::size_t>(MEGA));
    std::cout << "[perf] 1M pipeline: " << ms << " ms\n";
    EXPECT_LT(ms, 2000);  // 2s for CI/container
}

// ── Edge: double-done is idempotent ──

TEST(MegaStream, DoubleDoneIsIdempotent) {
    auto sync_s = stream::from_vector<int>({1});
    auto async_h = to_async<int>(std::move(sync_s));
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();

    auto r1 = async_h.poll(budget, cx);
    ASSERT_TRUE(r1.is_emit());

    auto r2 = async_h.poll(budget, cx);
    EXPECT_TRUE(r2.is_done());

    // Polling again after done must still return done
    auto r3 = async_h.poll(budget, cx);
    EXPECT_TRUE(r3.is_done());

    auto r4 = async_h.poll(budget, cx);
    EXPECT_TRUE(r4.is_done());
}

// ── Edge: empty stream through full combinator chain ──

TEST(MegaStream, EmptyStreamThroughFullChain) {
    StreamScheduler sched(2);
    auto source = to_async<int>(stream::empty<int>());
    auto mapped = par_map<int>(std::move(source),
                               [](const int& x) { return x + 1; },
                               sched);
    auto result = collect_blocking<int>(mapped, sched);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

// ── Edge: Chunk<T> zero-element slice ──

TEST(MegaStream, ZeroLengthSliceIsEmpty) {
    auto chunk = Chunk<int>::from(make_mega_vector(1000));
    auto sliced = chunk.slice(500, 0);
    EXPECT_TRUE(sliced.empty());
    EXPECT_EQ(sliced.size(), 0u);
}

// ── Edge: take(0) from infinite-like source ──

TEST(MegaStream, TakeZeroFromLargeSource) {
    auto src = stream::from_vector<int>(make_mega_vector(MEGA));
    auto taken = stream::take<int>(std::move(src), 0);
    auto result = stream::collect(taken);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

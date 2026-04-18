/// tests/test_core_coverage.cpp — focused coverage tests for core dispatch,
/// scheduler, async/sync stream combinators, and api routing.
///
/// Uses a backend-agnostic in-memory store so we can exercise composite-tree
/// dispatch (node_del, node_prefix_scan, node_batch, node_compact, node_foreach,
/// fan-out scans) without depending on RocksDB/SQLite I/O.

#include "celer/celer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace celer;
using namespace celer::async;

// ════════════════════════════════════════
// InMemoryBackend — pure StorageBackend for unit tests
// ════════════════════════════════════════

namespace {

class InMemoryBackend {
public:
    static constexpr auto name() noexcept -> std::string_view { return "memory"; }

    auto get(std::string_view key) -> Result<std::optional<std::string>> {
        if (auto it = data_.find(std::string(key)); it != data_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }

    auto put(std::string_view key, std::string_view value) -> VoidResult {
        data_[std::string(key)] = std::string(value);
        return {};
    }

    auto del(std::string_view key) -> VoidResult {
        data_.erase(std::string(key));
        return {};
    }

    auto prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
        std::vector<KVPair> out;
        for (const auto& [k, v] : data_) {
            if (k.compare(0, prefix.size(), prefix) == 0) {
                out.push_back({k, v});
            }
        }
        return out;
    }

    auto batch(std::span<const BatchOp> ops) -> VoidResult {
        for (const auto& op : ops) {
            if (op.kind == BatchOp::Kind::put) {
                data_[op.key] = op.value.value_or("");
            } else {
                data_.erase(op.key);
            }
        }
        return {};
    }

    auto compact() -> VoidResult {
        ++compact_calls_;
        return {};
    }

    auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* ctx) -> VoidResult {
        for (const auto& [k, v] : data_) {
            if (k.compare(0, prefix.size(), prefix) == 0) {
                visitor(ctx, k, v);
            }
        }
        return {};
    }

    // Stream extensions — provide minimal valid implementations to satisfy concept.
    auto stream_get(std::string_view key) -> Result<StreamHandle<char>> {
        auto it = data_.find(std::string(key));
        if (it == data_.end()) {
            return stream::from_string(std::string{});
        }
        return stream::from_string(it->second);
    }

    auto stream_put(std::string_view key, StreamHandle<char> source) -> VoidResult {
        auto bytes = stream::collect_string(source);
        if (!bytes) return std::unexpected(bytes.error());
        data_[std::string(key)] = std::move(*bytes);
        return {};
    }

    auto stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>> {
        std::vector<KVPair> pairs;
        for (const auto& [k, v] : data_) {
            if (k.compare(0, prefix.size(), prefix) == 0) {
                pairs.push_back({k, v});
            }
        }
        return stream::from_vector<KVPair>(std::move(pairs));
    }

    [[nodiscard]] auto compact_calls() const noexcept -> int { return compact_calls_; }

private:
    std::map<std::string, std::string> data_;
    int compact_calls_{0};
};

static_assert(StorageBackend<InMemoryBackend>);

auto make_memory_backend() -> BackendHandle {
    return make_backend_handle(new InMemoryBackend{});
}

auto build_two_leaf_composite(const char* composite_name = "project")
    -> StoreNode {
    auto h_a = make_memory_backend();
    auto h_b = make_memory_backend();
    auto leaf_a = build_leaf("alpha", std::move(h_a));
    auto leaf_b = build_leaf("beta",  std::move(h_b));
    EXPECT_TRUE(leaf_a.has_value());
    EXPECT_TRUE(leaf_b.has_value());
    std::vector<StoreNode> children;
    children.push_back(std::move(*leaf_a));
    children.push_back(std::move(*leaf_b));
    auto comp = build_composite(composite_name, std::move(children));
    EXPECT_TRUE(comp.has_value());
    return std::move(*comp);
}

} // namespace

// ════════════════════════════════════════
// dispatch.hpp — route_key branches
// ════════════════════════════════════════

TEST(RouteKey, ColonSplitsChildAndRest) {
    auto r = celer::detail::route_key("alpha:foo:bar");
    EXPECT_EQ(r.child, "alpha");
    EXPECT_EQ(r.rest, "foo:bar");
}

TEST(RouteKey, NoColonReturnsWholeKeyAsChild) {
    auto r = celer::detail::route_key("plainkey");
    EXPECT_EQ(r.child, "plainkey");
    EXPECT_EQ(r.rest, "");
}

TEST(RouteKey, EmptyKey) {
    auto r = celer::detail::route_key("");
    EXPECT_EQ(r.child, "");
    EXPECT_EQ(r.rest, "");
}

// ════════════════════════════════════════
// dispatch.cpp — composite-routed mutations
// ════════════════════════════════════════

TEST(NodeDispatch, DelOnLeafAndComposite) {
    auto comp = build_two_leaf_composite();

    ASSERT_TRUE(node_put(comp, "alpha:k1", "v1").has_value());
    ASSERT_TRUE(node_put(comp, "beta:k1",  "v2").has_value());

    auto pre = node_get(comp, "alpha:k1");
    ASSERT_TRUE(pre.has_value() && pre->has_value());

    auto del_ok = node_del(comp, "alpha:k1");
    ASSERT_TRUE(del_ok.has_value());

    auto post = node_get(comp, "alpha:k1");
    ASSERT_TRUE(post.has_value());
    EXPECT_FALSE(post->has_value());

    // beta still present
    auto beta = node_get(comp, "beta:k1");
    ASSERT_TRUE(beta.has_value() && beta->has_value());
    EXPECT_EQ(beta->value(), "v2");

    // Del routed to non-existent child
    auto bad = node_del(comp, "nosuch:k");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, "ChildNotFound");
}

TEST(NodeDispatch, PrefixScanRoutedAndFanout) {
    auto comp = build_two_leaf_composite();

    ASSERT_TRUE(node_put(comp, "alpha:a1", "x").has_value());
    ASSERT_TRUE(node_put(comp, "alpha:a2", "y").has_value());
    ASSERT_TRUE(node_put(comp, "beta:b1",  "z").has_value());

    // Routed scan: prefix has child name → narrow to that child only.
    auto only_alpha = node_prefix_scan(comp, "alpha:a");
    ASSERT_TRUE(only_alpha.has_value());
    EXPECT_EQ(only_alpha->size(), 2u);
    for (const auto& kv : *only_alpha) {
        // After routing, leaf scan returned its own keys (not prefixed).
        EXPECT_TRUE(kv.key.starts_with("a"));
    }

    // Fan-out scan: empty prefix touches every leaf.
    auto all = node_prefix_scan(comp, "");
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->size(), 3u);

    // Routed scan into a non-existent child surfaces the error.
    auto bad = node_prefix_scan(comp, "nosuch:k");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, "ChildNotFound");
}

TEST(NodeDispatch, BatchGroupedByCfNameRoutesToChildren) {
    auto comp = build_two_leaf_composite();

    std::vector<BatchOp> ops{
        {BatchOp::Kind::put, "alpha", "k1", std::string{"v_a1"}},
        {BatchOp::Kind::put, "alpha", "k2", std::string{"v_a2"}},
        {BatchOp::Kind::put, "beta",  "k1", std::string{"v_b1"}},
        {BatchOp::Kind::del, "alpha", "missing", std::nullopt},
    };
    ASSERT_TRUE(node_batch(comp, ops).has_value());

    auto a1 = node_get(comp, "alpha:k1");
    ASSERT_TRUE(a1.has_value() && a1->has_value());
    EXPECT_EQ(a1->value(), "v_a1");

    auto b1 = node_get(comp, "beta:k1");
    ASSERT_TRUE(b1.has_value() && b1->has_value());
    EXPECT_EQ(b1->value(), "v_b1");

    // Bad cf_name routes to ChildNotFound
    std::vector<BatchOp> bad_ops{
        {BatchOp::Kind::put, "nosuch", "k", std::string{"v"}},
    };
    auto bad = node_batch(comp, bad_ops);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, "ChildNotFound");
}

TEST(NodeDispatch, CompactRecursivelyVisitsAllLeaves) {
    // Build a 2-deep tree so the recursive composite branch runs.
    std::vector<StoreNode> inner_children;
    auto leaf_a = build_leaf("alpha", make_memory_backend());
    auto leaf_b = build_leaf("beta",  make_memory_backend());
    ASSERT_TRUE(leaf_a.has_value() && leaf_b.has_value());
    inner_children.push_back(std::move(*leaf_a));
    inner_children.push_back(std::move(*leaf_b));
    auto inner = build_composite("inner", std::move(inner_children));
    ASSERT_TRUE(inner.has_value());

    std::vector<StoreNode> outer_children;
    outer_children.push_back(std::move(*inner));
    auto outer = build_composite("outer", std::move(outer_children));
    ASSERT_TRUE(outer.has_value());

    // Composite compact should fan-out without error.
    ASSERT_TRUE(node_compact(*outer).has_value());

    // Direct leaf compact also exercises the leaf branch.
    auto solo = build_leaf("solo", make_memory_backend());
    ASSERT_TRUE(solo.has_value());
    ASSERT_TRUE(node_compact(*solo).has_value());
}

TEST(NodeDispatch, ForeachRoutedAndFanoutAndError) {
    auto comp = build_two_leaf_composite();
    ASSERT_TRUE(node_put(comp, "alpha:k1", "x").has_value());
    ASSERT_TRUE(node_put(comp, "alpha:k2", "y").has_value());
    ASSERT_TRUE(node_put(comp, "beta:k1",  "z").has_value());

    // Routed foreach
    int count_a = 0;
    auto r1 = node_foreach(comp, "alpha:",
        [](void* c, std::string_view, std::string_view) {
            ++(*static_cast<int*>(c));
        }, &count_a);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(count_a, 2);

    // Fan-out foreach (empty prefix)
    int count_all = 0;
    auto r2 = node_foreach(comp, "",
        [](void* c, std::string_view, std::string_view) {
            ++(*static_cast<int*>(c));
        }, &count_all);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(count_all, 3);

    // Routed to non-existent child → error
    int unused = 0;
    auto r3 = node_foreach(comp, "nosuch:k",
        [](void*, std::string_view, std::string_view) {}, &unused);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().code, "ChildNotFound");
}

// ════════════════════════════════════════
// api/store.hpp + api/db_ref.hpp branches
// ════════════════════════════════════════

TEST(Store, RootIsLeafReturnsInvalidRoot) {
    auto leaf = build_leaf("solo", make_memory_backend());
    ASSERT_TRUE(leaf.has_value());
    Store store{std::move(*leaf), ResourceStack{}};
    auto db = store.db("anything");
    ASSERT_FALSE(db.has_value());
    EXPECT_EQ(db.error().code, "InvalidRoot");
}

TEST(DbRef, NullNodeYieldsError) {
    DbRef ref{"empty", nullptr};
    auto t = ref.table("any");
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().code, "DbRef");

    auto s = ref.scan_all("p");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, "DbRef");

    std::vector<BatchOp> ops;
    auto b = ref.batch(ops);
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().code, "DbRef");
}

TEST(DbRef, TableOnDirectLeafScopeIgnoresName) {
    // Scope itself is a leaf — table_name is ignored, returns TableRef
    // backed by that leaf's handle. Build a tree where "scope_leaf" is a leaf
    // directly under the root composite.
    auto leaf = build_leaf("scope_leaf", make_memory_backend());
    ASSERT_TRUE(leaf.has_value());
    std::vector<StoreNode> roots;
    roots.push_back(std::move(*leaf));
    auto root = build_composite("root", std::move(roots));
    ASSERT_TRUE(root.has_value());

    Store store{std::move(*root), ResourceStack{}};
    auto db = store.db("scope_leaf");
    ASSERT_TRUE(db.has_value());

    auto tbl = db->table("ignored_name");
    ASSERT_TRUE(tbl.has_value());

    ASSERT_TRUE(tbl->put_raw("k", "v").has_value());
    auto got = tbl->get_raw("k");
    ASSERT_TRUE(got.has_value() && got->has_value());
    EXPECT_EQ(got->value(), "v");
}

TEST(DbRef, TableThatIsCompositeReturnsError) {
    // scope (composite) → child (composite) → leaves
    auto inner_leaf = build_leaf("inner", make_memory_backend());
    ASSERT_TRUE(inner_leaf.has_value());
    std::vector<StoreNode> child_kids;
    child_kids.push_back(std::move(*inner_leaf));
    auto sub_comp = build_composite("sub", std::move(child_kids));
    ASSERT_TRUE(sub_comp.has_value());

    std::vector<StoreNode> scope_kids;
    scope_kids.push_back(std::move(*sub_comp));  // child "sub" is itself a composite
    auto scope = build_composite("scope", std::move(scope_kids));
    ASSERT_TRUE(scope.has_value());

    std::vector<StoreNode> roots;
    roots.push_back(std::move(*scope));
    auto root = build_composite("root", std::move(roots));
    ASSERT_TRUE(root.has_value());

    Store store{std::move(*root), ResourceStack{}};
    auto db = store.db("scope");
    ASSERT_TRUE(db.has_value());

    auto bad = db->table("sub");  // "sub" is a composite, not a leaf
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, "TableIsComposite");
}

TEST(DbRef, ScanAllAndBatchThroughDbRef) {
    auto comp = build_two_leaf_composite();
    std::vector<StoreNode> roots;
    roots.push_back(std::move(comp));
    auto root = build_composite("root", std::move(roots));
    ASSERT_TRUE(root.has_value());

    Store store{std::move(*root), ResourceStack{}};
    auto db = store.db("project");
    ASSERT_TRUE(db.has_value());

    // batch via DbRef
    std::vector<BatchOp> ops{
        {BatchOp::Kind::put, "alpha", "k1", std::string{"x"}},
        {BatchOp::Kind::put, "beta",  "k1", std::string{"y"}},
    };
    ASSERT_TRUE(db->batch(ops).has_value());

    // scan_all fan-out
    auto scan = db->scan_all("");
    ASSERT_TRUE(scan.has_value());
    EXPECT_EQ(scan->size(), 2u);
}

// ════════════════════════════════════════
// stream.hpp — uncovered combinator paths
// ════════════════════════════════════════

TEST(Stream, FromStringAndCollectString) {
    auto s = stream::from_string("hello world");
    ASSERT_TRUE(s.valid());
    auto collected = stream::collect_string(s);
    ASSERT_TRUE(collected.has_value());
    EXPECT_EQ(*collected, "hello world");
}

TEST(Stream, FromStringEmptyIsImmediatelyDone) {
    auto s = stream::from_string("");
    auto collected = stream::collect_string(s);
    ASSERT_TRUE(collected.has_value());
    EXPECT_TRUE(collected->empty());
}

TEST(Stream, MapClonePreservesPosition) {
    auto src = stream::from_vector<int>({1, 2, 3, 4});
    auto mapped = stream::map(std::move(src), [](const int& x) { return x + 10; });

    auto cloned = mapped.clone();

    auto v1 = stream::collect(mapped);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, std::vector<int>({11, 12, 13, 14}));

    // Clone is independent — same starting position.
    auto v2 = stream::collect(cloned);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, std::vector<int>({11, 12, 13, 14}));
}

TEST(Stream, FilterClonePreservesPredicateAndPosition) {
    auto src = stream::from_vector<int>({1, 2, 3, 4, 5});
    auto filtered = stream::filter(std::move(src), [](const int& x) { return x % 2 == 0; });
    auto cloned = filtered.clone();

    auto v1 = stream::collect(filtered);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, std::vector<int>({2, 4}));

    auto v2 = stream::collect(cloned);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, std::vector<int>({2, 4}));
}

TEST(Stream, TakeClonePreservesRemaining) {
    auto src = stream::from_vector<int>({1, 2, 3, 4, 5});
    auto taken = stream::take(std::move(src), 3);
    auto cloned = taken.clone();

    auto v1 = stream::collect(taken);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, std::vector<int>({1, 2, 3}));

    auto v2 = stream::collect(cloned);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, std::vector<int>({1, 2, 3}));
}

TEST(Stream, TakeSlicesChunkWhenLargerThanRemaining) {
    // Single chunk of 5 elements, take 2 → exercises the slice path
    auto src = stream::from_vector<int>({10, 20, 30, 40, 50});
    auto taken = stream::take(std::move(src), 2);
    auto v = stream::collect(taken);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::vector<int>({10, 20}));
}

TEST(Stream, ChunkSliceEmptyAndOutOfBounds) {
    auto chunk = Chunk<int>::from(std::vector<int>{1, 2, 3, 4});
    // Zero-length slice
    auto empty = chunk.slice(1, 0);
    EXPECT_TRUE(empty.empty());

    // Offset >= size
    auto past = chunk.slice(10, 5);
    EXPECT_TRUE(past.empty());

    // Length truncated to remaining
    auto trunc = chunk.slice(2, 100);
    EXPECT_EQ(trunc.size(), 2u);
    EXPECT_EQ(trunc[0], 3);
}

TEST(Stream, StreamHandleCloneOfDefaultIsInvalid) {
    StreamHandle<int> empty;
    auto cloned = empty.clone();
    EXPECT_FALSE(cloned.valid());
}

// ════════════════════════════════════════
// async_stream.hpp — request / clone / error paths
// ════════════════════════════════════════

TEST(AsyncStreamHandle, RequestAndCancelDelegateToImpl) {
    // SyncBridgeImpl::request is a no-op; SyncBridgeImpl::cancel sets done.
    // After cancel(), poll() must return Done.
    auto async_h = to_async<int>(stream::from_vector<int>({1, 2, 3}));

    // Exercise the request_fn vtable path (SyncBridge ignores n).
    async_h.request(8);

    async_h.cancel();
    TaskContext cx{0, nullptr};
    auto budget = StreamBudget::unbounded();
    EXPECT_TRUE(async_h.poll(budget, cx).is_done());
}

TEST(AsyncStreamHandle, RequestOnDefaultHandleIsSafe) {
    AsyncStreamHandle<int> empty;
    // No-op; must not crash.
    empty.request(4);
    empty.cancel();
    EXPECT_FALSE(empty.valid());
}

namespace {

// A controllable async stream that emits a fixed sequence of PollResults.
template <typename T>
struct ScriptedAsync {
    std::vector<PollResult<T>> script;
    std::size_t idx{0};
    std::size_t request_count{0};
    bool cancelled{false};

    auto poll(StreamBudget, TaskContext&) -> PollResult<T> {
        if (idx >= script.size()) return PollResult<T>::done();
        // PollResults are not copyable in general — emit holds a Chunk which is
        // copyable, error holds an Error which is copyable. Move-out by index.
        PollResult<T> r = std::move(script[idx]);
        ++idx;
        return r;
    }

    void request(std::size_t n) { request_count += n; }
    void cancel() { cancelled = true; }

    ScriptedAsync() = default;
    ScriptedAsync(const ScriptedAsync&) = default;
};

template <typename T>
auto make_scripted(std::vector<PollResult<T>> script) -> AsyncStreamHandle<T> {
    auto* impl = new ScriptedAsync<T>{};
    impl->script = std::move(script);
    return make_async_stream_handle<T>(impl);
}

} // namespace

TEST(Combinators, ParMapHandlesPendingYieldErrorBranches) {
    StreamScheduler sched(1);

    {
        std::vector<PollResult<int>> script;
        script.push_back(PollResult<int>::pending());
        auto src = make_scripted<int>(std::move(script));
        auto mapped = par_map<int>(std::move(src), [](const int& x) { return x; }, sched, 1);

        TaskContext cx{0, nullptr};
        auto r = mapped.poll(StreamBudget::unbounded(), cx);
        EXPECT_TRUE(r.is_pending());
    }
    {
        std::vector<PollResult<int>> script;
        script.push_back(PollResult<int>::yield());
        auto src = make_scripted<int>(std::move(script));
        auto mapped = par_map<int>(std::move(src), [](const int& x) { return x; }, sched, 1);

        TaskContext cx{0, nullptr};
        auto r = mapped.poll(StreamBudget::unbounded(), cx);
        EXPECT_TRUE(r.is_yield());
    }
    {
        std::vector<PollResult<int>> script;
        script.push_back(PollResult<int>::err(Error{"E", "boom"}));
        auto src = make_scripted<int>(std::move(script));
        auto mapped = par_map<int>(std::move(src), [](const int& x) { return x; }, sched, 1);

        TaskContext cx{0, nullptr};
        auto r = mapped.poll(StreamBudget::unbounded(), cx);
        ASSERT_TRUE(r.is_error());
        EXPECT_EQ(r.error.code, "E");
    }
}

TEST(Combinators, ParMapRequestForwardsAndCloneIndependent) {
    StreamScheduler sched(1);
    auto src = to_async<int>(stream::from_vector<int>({1, 2, 3}));
    auto mapped = par_map<int>(std::move(src),
                               [](const int& x) { return x * 10; },
                               sched, 1);

    // Just confirm request() and clone() do not crash and clone yields equal results.
    mapped.request(4);
    auto cloned = mapped.clone();

    auto v_orig = collect_blocking<int>(mapped, sched);
    ASSERT_TRUE(v_orig.has_value());
    EXPECT_EQ(*v_orig, std::vector<int>({10, 20, 30}));

    auto v_clone = collect_blocking<int>(cloned, sched);
    ASSERT_TRUE(v_clone.has_value());
    EXPECT_EQ(*v_clone, std::vector<int>({10, 20, 30}));
}

TEST(Combinators, ParEvalRequestCancelClone) {
    StreamScheduler sched(1);
    std::vector<AsyncStreamHandle<int>> srcs;
    srcs.push_back(to_async<int>(stream::from_vector<int>({1, 2, 3})));
    srcs.push_back(to_async<int>(stream::from_vector<int>({4, 5, 6})));
    auto merged = par_eval<int>(std::move(srcs), sched);

    // Forward request to all sources.
    merged.request(8);

    // Clone yields an equivalent independent stream.
    auto cloned = merged.clone();

    auto v = collect_blocking<int>(merged, sched);
    ASSERT_TRUE(v.has_value());
    auto out = *v;
    std::sort(out.begin(), out.end());
    EXPECT_EQ(out, std::vector<int>({1, 2, 3, 4, 5, 6}));

    // Cancel + collect returns whatever was buffered (possibly empty).
    cloned.cancel();
    (void)collect_blocking<int>(cloned, sched);
}

TEST(Combinators, ParEvalEmptySourcesIsDone) {
    StreamScheduler sched(1);
    std::vector<AsyncStreamHandle<int>> empty_srcs;
    auto merged = par_eval<int>(std::move(empty_srcs), sched);
    auto v = collect_blocking<int>(merged, sched);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->empty());
}

TEST(Combinators, CollectBlockingHandlesPendingYieldThenDone) {
    StreamScheduler sched(1);

    std::vector<PollResult<int>> script;
    script.push_back(PollResult<int>::pending());
    script.push_back(PollResult<int>::yield());
    script.push_back(PollResult<int>::emit(Chunk<int>::from(std::vector<int>{7, 8})));
    script.push_back(PollResult<int>::done());

    auto h = make_scripted<int>(std::move(script));
    auto v = collect_blocking<int>(h, sched);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::vector<int>({7, 8}));
}

TEST(Combinators, CollectBlockingPropagatesError) {
    StreamScheduler sched(1);

    std::vector<PollResult<int>> script;
    script.push_back(PollResult<int>::err(Error{"BadStream", "fail"}));
    auto h = make_scripted<int>(std::move(script));

    auto v = collect_blocking<int>(h, sched);
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, "BadStream");
}

// ════════════════════════════════════════
// scheduler.hpp — full worker dispatch via set_poll_fn
// ════════════════════════════════════════

namespace {

// A tiny pollable: emits N times, then Done. Tracks polls/cancels via atomics.
struct CountingPollable {
    std::atomic<int>* polls;
    std::atomic<int>* dones;
    std::atomic<int> remaining;

    CountingPollable(std::atomic<int>* p, std::atomic<int>* d, int n)
        : polls(p), dones(d), remaining(n) {}
};

// Type-erased poll function: PollKind::Emit while remaining > 0, then Done.
auto counting_poll(void* ctx, const void* /*vt*/, StreamBudget, TaskContext&) -> PollKind {
    auto* p = static_cast<CountingPollable*>(ctx);
    p->polls->fetch_add(1, std::memory_order_relaxed);
    if (p->remaining.fetch_sub(1, std::memory_order_relaxed) > 0) {
        return PollKind::Emit;
    }
    p->dones->fetch_add(1, std::memory_order_relaxed);
    return PollKind::Done;
}

} // namespace

TEST(Scheduler, ScheduleAndDispatchEmitDoneViaWorkerLoop) {
    StreamScheduler sched(2);
    sched.set_poll_fn(counting_poll);

    std::atomic<int> polls{0}, dones{0};
    auto pollable = std::make_unique<CountingPollable>(&polls, &dones, /*n=*/3);
    auto control = std::make_unique<StreamControl>();
    control->request(/*n=*/100);  // must be > 0 so should_advance() == true

    StreamLease lease;
    lease.stream_ctx = pollable.get();
    lease.vtable = reinterpret_cast<const void*>(0x1);  // non-null sentinel
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    // Wait for the lease to be fully consumed.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dones.load(std::memory_order_relaxed) == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();

    EXPECT_GE(polls.load(), 4);  // 3 Emit polls + 1 Done poll
    EXPECT_EQ(dones.load(), 1);
}

TEST(Scheduler, ScheduleAffineToValidWorker) {
    StreamScheduler sched(2);
    sched.set_poll_fn(counting_poll);

    std::atomic<int> polls{0}, dones{0};
    auto pollable = std::make_unique<CountingPollable>(&polls, &dones, /*n=*/1);
    auto control = std::make_unique<StreamControl>();
    control->request(10);

    StreamLease lease;
    lease.stream_ctx = pollable.get();
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule_affine(std::move(lease), /*worker_id=*/0);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dones.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();
    EXPECT_GE(dones.load(), 1);
}

TEST(Scheduler, ScheduleAffineFallsBackWhenWorkerIdInvalid) {
    StreamScheduler sched(2);
    sched.set_poll_fn(counting_poll);

    std::atomic<int> polls{0}, dones{0};
    auto pollable = std::make_unique<CountingPollable>(&polls, &dones, /*n=*/1);
    auto control = std::make_unique<StreamControl>();
    control->request(10);

    StreamLease lease;
    lease.stream_ctx = pollable.get();
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    // worker_id >= num_workers → falls back to global queue
    sched.schedule_affine(std::move(lease), /*worker_id=*/99);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dones.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();
    EXPECT_GE(dones.load(), 1);
}

TEST(Scheduler, ParkOnPendingThenWakeRescheduleViaCallback) {
    StreamScheduler sched(2);

    // Custom poll_fn: first call returns Pending (parks), then Done after wake.
    static std::atomic<int> phase{0};
    static std::atomic<int> dones{0};
    phase = 0;
    dones = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        if (phase.fetch_add(1) == 0) {
            return PollKind::Pending;
        }
        dones.fetch_add(1);
        return PollKind::Done;
    });

    auto control = std::make_unique<StreamControl>();
    control->request(10);

    // Stable, non-null stream_ctx so wake() can match it.
    int sentinel = 0;

    StreamLease lease;
    lease.stream_ctx = &sentinel;
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    // Wait until lease is parked.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sched.parked_count() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(sched.parked_count(), 1u);

    // Wake → rescheduled, second poll returns Done.
    sched.wake(&sentinel);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dones.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();
    EXPECT_GE(dones.load(), 1);
}

TEST(Scheduler, CancelledLeaseIsReapedNotPolled) {
    StreamScheduler sched(1);

    static std::atomic<int> calls{0};
    calls = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        calls.fetch_add(1);
        return PollKind::Done;
    });

    auto control = std::make_unique<StreamControl>();
    control->request(1);
    control->cancel();  // pre-cancelled

    int sentinel = 0;
    StreamLease lease;
    lease.stream_ctx = &sentinel;
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    // Give the worker a moment to drain the queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sched.shutdown();

    EXPECT_EQ(calls.load(), 0);
}

TEST(Scheduler, NoDemandLeaseIsParkedNotPolled) {
    StreamScheduler sched(1);

    static std::atomic<int> calls{0};
    calls = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        calls.fetch_add(1);
        return PollKind::Done;
    });

    auto control = std::make_unique<StreamControl>();
    // No request() call → demand is 0 → should_advance() == false → parks.

    int sentinel = 0;
    StreamLease lease;
    lease.stream_ctx = &sentinel;
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sched.parked_count() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(sched.parked_count(), 1u);
    EXPECT_EQ(calls.load(), 0);
    sched.shutdown();
}

TEST(StreamLease, ValidRequiresAllNonNull) {
    StreamLease l;
    EXPECT_FALSE(l.valid());

    int x = 0;
    StreamControl ctl;
    l.stream_ctx = &x;
    EXPECT_FALSE(l.valid());
    l.vtable = reinterpret_cast<const void*>(0x1);
    EXPECT_FALSE(l.valid());
    l.control = &ctl;
    EXPECT_TRUE(l.valid());
}

TEST(ParkingLot, WakeMissingReturnsNullopt) {
    ParkingLot lot;
    int phantom = 0;
    EXPECT_FALSE(lot.wake(&phantom).has_value());

    int real = 0;
    StreamLease l;
    l.stream_ctx = &real;
    lot.park(std::move(l));
    EXPECT_EQ(lot.count(), 1u);

    auto found = lot.wake(&real);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->stream_ctx, &real);
    EXPECT_EQ(lot.count(), 0u);
}

// ════════════════════════════════════════
// Additional coverage — stream / scheduler edge paths
// ════════════════════════════════════════

TEST(Stream, PullOnDefaultHandleReturnsError) {
    StreamHandle<int> empty;
    auto r = empty.pull();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, "StreamPull");
}

TEST(Stream, ChunkCopyAssignmentAndCopyConstructor) {
    auto a = Chunk<int>::from(std::vector<int>{1, 2, 3});
    Chunk<int> b = a;          // copy ctor
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b[1], 2);

    Chunk<int> c;
    c = a;                     // copy assignment
    EXPECT_EQ(c.size(), 3u);
    EXPECT_EQ(c[2], 3);

    // Self-copy-assign (should be a no-op safety check).
    c = c;
    EXPECT_EQ(c.size(), 3u);
}

TEST(Stream, TakeWhenChunkFitsRemaining) {
    // Use multiple singleton chunks so take's "chunk.size() <= remaining" path
    // is exercised (it returns the chunk wholesale instead of slicing).
    auto src = stream::singleton<int>(42);
    auto taken = stream::take(std::move(src), 5);  // remaining > chunk size
    auto v = stream::collect(taken);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::vector<int>({42}));
}

TEST(Stream, FromStringImplCopyConstructorViaClone) {
    auto s = stream::from_string("abc");
    auto cloned = s.clone();
    auto a = stream::collect_string(s);
    auto b = stream::collect_string(cloned);
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_EQ(*a, "abc");
    EXPECT_EQ(*b, "abc");
}

TEST(Scheduler, DispatchYieldRequeues) {
    // Custom poll_fn: returns Yield once, then Done. The Yield case requeues
    // the lease locally — the second poll observes Done.
    StreamScheduler sched(1);
    static std::atomic<int> phase{0};
    static std::atomic<int> done_count{0};
    phase = 0;
    done_count = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        if (phase.fetch_add(1) == 0) return PollKind::Yield;
        done_count.fetch_add(1);
        return PollKind::Done;
    });

    auto control = std::make_unique<StreamControl>();
    control->request(10);

    int sentinel = 0;
    StreamLease lease;
    lease.stream_ctx = &sentinel;
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (done_count.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();
    EXPECT_GE(done_count.load(), 1);
    EXPECT_GE(phase.load(), 2);  // Yield + Done
}

TEST(Scheduler, DispatchErrorReleasesLease) {
    StreamScheduler sched(1);
    static std::atomic<int> calls{0};
    calls = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        calls.fetch_add(1);
        return PollKind::Error;
    });

    auto control = std::make_unique<StreamControl>();
    control->request(10);

    int sentinel = 0;
    StreamLease lease;
    lease.stream_ctx = &sentinel;
    lease.vtable = reinterpret_cast<const void*>(0x1);
    lease.budget = StreamBudget::local();
    lease.control = control.get();

    sched.schedule(std::move(lease));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (calls.load() == 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sched.shutdown();
    EXPECT_GE(calls.load(), 1);
}

TEST(Scheduler, DrainRemainingOnShutdown) {
    // Push many pending leases onto a worker, then shut down before they're
    // all consumed. The drain_remaining() path invokes poll_fn for each lease
    // still in the local deque.
    StreamScheduler sched(1);
    static std::atomic<int> calls{0};
    calls = 0;
    sched.set_poll_fn(+[](void*, const void*, StreamBudget, TaskContext&) -> PollKind {
        calls.fetch_add(1);
        return PollKind::Done;
    });

    std::vector<std::unique_ptr<StreamControl>> controls;
    for (int i = 0; i < 32; ++i) {
        auto control = std::make_unique<StreamControl>();
        control->request(10);
        int sentinel_addr = i;  // any non-null distinct address-bearer
        StreamLease lease;
        lease.stream_ctx = &controls;  // any non-null
        lease.vtable = reinterpret_cast<const void*>(0x1);
        lease.budget = StreamBudget::local();
        lease.control = control.get();
        sched.schedule_affine(std::move(lease), 0);
        controls.push_back(std::move(control));
        (void)sentinel_addr;
    }
    sched.shutdown();
    // All leases should have been drained and polled.
    EXPECT_GE(calls.load(), 1);
}

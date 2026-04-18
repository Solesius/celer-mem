#pragma once
/// celer::materialization::join — cross-store join executor.
///
/// Public surface:
///   join(left_stream, right_store, key_extractor, opts)
///       -> RecordStream<Joined<L, R>>
///
/// All five strategies live in this header (templates inline). The chosen
/// strategy is materialized lazily — the returned RecordStream pulls from
/// a per-strategy impl that holds left source + right store + state.
///
/// Design hot-path notes:
///   • BatchIndexNestedLoop is the default. It buffers a chunk of left
///     records, dedupes their right-keys via flat_hash_map of FNV-1a
///     fingerprints, calls right.get_many(unique_keys) once, then probes.
///   • HashJoin builds the smaller side fully into a flat_hash_map keyed by
///     FNV-1a, then streams the larger side and probes.
///   • MergeJoin and NestedLoop are correctness-first; not the fast path.
///   • IndexNestedLoop is the BatchIndexNL degenerate case (batch_size=1).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"
#include "celer/materialization/codec.hpp"
#include "celer/materialization/planner.hpp"
#include "celer/materialization/store_ref.hpp"
#include "celer/materialization/stream.hpp"

namespace celer::materialization {

// ── Joined output ──

template <typename L, typename R>
struct Joined {
    std::string         key;       ///< right-side key the join matched on
    L                   left;
    std::optional<R>    right;     ///< nullopt only for outer joins
};

namespace detail {

// ── Hash map alias (no abseil dependency; std unordered is fine for v1) ──
template <typename K, typename V, typename H = KeyHash>
using DenseMap = std::unordered_map<K, V, H>;

// ── Helper: emit-policy for a (left_record, optional<R>) pair under JoinKind ──
template <typename L, typename R>
[[nodiscard]] inline auto should_emit(JoinKind k, bool right_present) noexcept -> bool {
    switch (k) {
        case JoinKind::Inner:      return right_present;
        case JoinKind::LeftOuter:  return true;
        case JoinKind::SemiLeft:   return right_present;
        case JoinKind::AntiLeft:   return !right_present;
        case JoinKind::RightOuter: return right_present;     // right-driven elsewhere
        case JoinKind::FullOuter:  return true;              // rights handled in pass2
    }
    return right_present;
}

// ── BatchIndexNestedLoop ─────────────────────────────────────────────────
//
// Buffers up to ``batch_size`` left records, derives right-side keys via
// ``Extractor``, dedupes keys with FNV-1a fingerprints, issues a single
// ``StoreRef<R>::get_many(...)`` call, then probes via ``DenseMap``.
template <typename L, typename R, typename Extractor>
struct BatchIndexNLImpl {
    StreamHandle<Record<L>>   left_src;
    StoreRef<R>               right;
    Extractor                 extract;
    JoinKind                  kind;
    std::size_t               batch_size;
    std::size_t               limit_remaining{0};
    bool                      has_limit{false};

    // Pending output queue for emitted Joined<L,R>; refilled by pull().
    std::vector<Joined<L, R>> pending_out;
    std::size_t               out_cursor{0};
    bool                      upstream_done{false};

    // Buffer of left records pulled but not yet processed (chunks may be
    // larger than batch_size; we slice them across refill calls).
    Chunk<Record<L>>          leftover;
    std::size_t               leftover_idx{0};

    BatchIndexNLImpl(StreamHandle<Record<L>> src,
                     StoreRef<R> r,
                     Extractor e,
                     JoinKind k,
                     std::size_t bs,
                     std::size_t lim)
        : left_src(std::move(src)), right(std::move(r))
        , extract(std::move(e)), kind(k), batch_size(bs)
        , limit_remaining(lim), has_limit(lim > 0) {}

    BatchIndexNLImpl(const BatchIndexNLImpl& o)
        : left_src(o.left_src.clone()), right(o.right), extract(o.extract)
        , kind(o.kind), batch_size(o.batch_size)
        , limit_remaining(o.limit_remaining), has_limit(o.has_limit)
        , pending_out(o.pending_out), out_cursor(o.out_cursor)
        , upstream_done(o.upstream_done)
        , leftover(o.leftover), leftover_idx(o.leftover_idx) {}

    // Pull next batch of left records, run the probe, fill pending_out.
    [[nodiscard]] auto refill() -> VoidResult {
        std::vector<Record<L>> left_batch;
        left_batch.reserve(batch_size);

        while (left_batch.size() < batch_size) {
            // First drain any leftover from prior pulls.
            if (leftover_idx < leftover.size()) {
                left_batch.push_back(leftover[leftover_idx++]);
                continue;
            }
            if (upstream_done) break;
            auto r = left_src.pull();
            if (!r) return std::unexpected(r.error());
            if (!r->has_value()) { upstream_done = true; break; }
            leftover     = r->value();
            leftover_idx = 0;
        }
        if (left_batch.empty()) return {};

        // Derive right-keys. We keep the raw bytes per-row (stable storage
        // for span<string_view> below) and parallel-index dedupe.
        std::vector<std::string>            row_keys;
        std::vector<std::size_t>            unique_idx;     // index into unique_keys per row
        DenseMap<Key, std::size_t>          dedupe;         // bytes -> unique slot
        std::vector<std::string>            unique_storage;
        row_keys.reserve(left_batch.size());
        unique_idx.reserve(left_batch.size());
        dedupe.reserve(left_batch.size() * 2);
        unique_storage.reserve(left_batch.size());

        for (auto& rec : left_batch) {
            auto enc = extract(rec);
            if (!enc) return std::unexpected(enc.error());
            row_keys.push_back(std::move(*enc));
            Key k{row_keys.back()};
            auto [it, inserted] = dedupe.try_emplace(k, unique_storage.size());
            if (inserted) {
                unique_storage.push_back(row_keys.back());
            }
            unique_idx.push_back(it->second);
        }

        // Single batch round-trip.
        std::vector<std::string_view> unique_views;
        unique_views.reserve(unique_storage.size());
        for (const auto& s : unique_storage) unique_views.emplace_back(s);

        auto fetched = right.get_many(unique_views);
        if (!fetched) return std::unexpected(fetched.error());

        // Inner-loop probe: branch is on JoinKind, hoisted out of the loop.
        const std::size_t n = left_batch.size();
        for (std::size_t i = 0; i < n; ++i) {
            const auto slot = unique_idx[i];
            auto& fetched_pair = (*fetched)[slot];
            const bool present = fetched_pair.second.has_value();
            if (!should_emit<L, R>(kind, present)) continue;

            Joined<L, R> j{
                .key = row_keys[i],
                .left = std::move(left_batch[i].value),
                .right = std::nullopt,
            };
            if (present) {
                // Copy not move — same R may match multiple left rows.
                j.right = fetched_pair.second;
            }
            pending_out.push_back(std::move(j));

            if (has_limit && pending_out.size() >= limit_remaining) {
                upstream_done = true;     // suppress further left pulls
                break;
            }
        }
        return {};
    }

    [[nodiscard]] auto pull() -> Result<std::optional<Chunk<Joined<L, R>>>> {
        while (out_cursor >= pending_out.size()) {
            if (upstream_done && pending_out.empty()) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
            pending_out.clear();
            out_cursor = 0;
            auto r = refill();
            if (!r) return std::unexpected(r.error());
            if (pending_out.empty() && upstream_done) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
        }
        // Drain pending_out as one chunk.
        std::vector<Joined<L, R>> drain(
            std::make_move_iterator(pending_out.begin() + out_cursor),
            std::make_move_iterator(pending_out.end()));
        out_cursor = pending_out.size();
        if (has_limit) {
            if (drain.size() > limit_remaining) drain.resize(limit_remaining);
            limit_remaining -= drain.size();
            if (limit_remaining == 0) upstream_done = true;
        }
        return std::optional{Chunk<Joined<L, R>>::from(std::move(drain))};
    }
};

// ── HashJoin ─────────────────────────────────────────────────────────────
//
// Builds the entire right side into a DenseMap<Key, R> first, then probes
// each left record. The build is bounded by ``hash_build_cap`` from JoinOptions
// (0 = unlimited). For now, build is eager and synchronous.
template <typename L, typename R, typename Extractor>
struct HashJoinImpl {
    StreamHandle<Record<L>>      left_src;
    DenseMap<Key, R>             right_index;
    Extractor                    extract;
    JoinKind                     kind;
    std::size_t                  limit_remaining{0};
    bool                         has_limit{false};
    std::vector<Joined<L, R>>    pending_out;
    std::size_t                  out_cursor{0};
    bool                         upstream_done{false};

    HashJoinImpl(StreamHandle<Record<L>> src,
                 DenseMap<Key, R> idx,
                 Extractor e,
                 JoinKind k,
                 std::size_t lim)
        : left_src(std::move(src)), right_index(std::move(idx))
        , extract(std::move(e)), kind(k)
        , limit_remaining(lim), has_limit(lim > 0) {}

    HashJoinImpl(const HashJoinImpl& o)
        : left_src(o.left_src.clone()), right_index(o.right_index)
        , extract(o.extract), kind(o.kind)
        , limit_remaining(o.limit_remaining), has_limit(o.has_limit)
        , pending_out(o.pending_out), out_cursor(o.out_cursor)
        , upstream_done(o.upstream_done) {}

    [[nodiscard]] auto refill() -> VoidResult {
        auto r = left_src.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) { upstream_done = true; return {}; }
        for (auto& rec : r->value()) {
            auto enc = extract(rec);
            if (!enc) return std::unexpected(enc.error());
            Key k{std::move(*enc)};
            auto it = right_index.find(k);
            const bool present = it != right_index.end();
            if (!should_emit<L, R>(kind, present)) continue;

            Joined<L, R> j{
                .key = k.bytes,
                .left = rec.value,
                .right = std::nullopt,
            };
            if (present) j.right = it->second;
            pending_out.push_back(std::move(j));

            if (has_limit && pending_out.size() >= limit_remaining) {
                upstream_done = true;
                break;
            }
        }
        return {};
    }

    [[nodiscard]] auto pull() -> Result<std::optional<Chunk<Joined<L, R>>>> {
        while (out_cursor >= pending_out.size()) {
            if (upstream_done && pending_out.empty()) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
            pending_out.clear();
            out_cursor = 0;
            auto r = refill();
            if (!r) return std::unexpected(r.error());
            if (pending_out.empty() && upstream_done) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
        }
        std::vector<Joined<L, R>> drain(
            std::make_move_iterator(pending_out.begin() + out_cursor),
            std::make_move_iterator(pending_out.end()));
        out_cursor = pending_out.size();
        if (has_limit) {
            if (drain.size() > limit_remaining) drain.resize(limit_remaining);
            limit_remaining -= drain.size();
            if (limit_remaining == 0) upstream_done = true;
        }
        return std::optional{Chunk<Joined<L, R>>::from(std::move(drain))};
    }
};

// ── IndexNestedLoop / NestedLoop / MergeJoin ────────────────────────────
//
// IndexNestedLoop is BatchIndexNL with batch_size = 1 (still uses get_many,
// but trivially). NestedLoop and MergeJoin are correctness-grade; they
// materialize the right side then iterate. Used as fallbacks only.

template <typename L, typename R, typename Extractor>
struct NestedLoopImpl {
    StreamHandle<Record<L>>     left_src;
    std::vector<Record<R>>      right_all;
    Extractor                   extract;
    JoinKind                    kind;
    std::size_t                 limit_remaining{0};
    bool                        has_limit{false};
    std::vector<Joined<L, R>>   pending_out;
    std::size_t                 out_cursor{0};
    bool                        upstream_done{false};

    NestedLoopImpl(StreamHandle<Record<L>> src,
                   std::vector<Record<R>> r,
                   Extractor e,
                   JoinKind k,
                   std::size_t lim)
        : left_src(std::move(src)), right_all(std::move(r)), extract(std::move(e))
        , kind(k), limit_remaining(lim), has_limit(lim > 0) {}

    NestedLoopImpl(const NestedLoopImpl& o)
        : left_src(o.left_src.clone()), right_all(o.right_all), extract(o.extract)
        , kind(o.kind), limit_remaining(o.limit_remaining), has_limit(o.has_limit)
        , pending_out(o.pending_out), out_cursor(o.out_cursor)
        , upstream_done(o.upstream_done) {}

    [[nodiscard]] auto refill() -> VoidResult {
        auto r = left_src.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) { upstream_done = true; return {}; }
        for (auto& rec : r->value()) {
            auto enc = extract(rec);
            if (!enc) return std::unexpected(enc.error());
            const auto& sought = *enc;
            std::optional<R> found;
            for (const auto& rr : right_all) {
                if (rr.key == sought) { found = rr.value; break; }
            }
            const bool present = found.has_value();
            if (!should_emit<L, R>(kind, present)) continue;

            Joined<L, R> j{ .key = sought, .left = rec.value, .right = std::move(found) };
            pending_out.push_back(std::move(j));
            if (has_limit && pending_out.size() >= limit_remaining) {
                upstream_done = true;
                break;
            }
        }
        return {};
    }

    [[nodiscard]] auto pull() -> Result<std::optional<Chunk<Joined<L, R>>>> {
        while (out_cursor >= pending_out.size()) {
            if (upstream_done && pending_out.empty()) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
            pending_out.clear();
            out_cursor = 0;
            auto r = refill();
            if (!r) return std::unexpected(r.error());
            if (pending_out.empty() && upstream_done) {
                return std::optional<Chunk<Joined<L, R>>>{};
            }
        }
        std::vector<Joined<L, R>> drain(
            std::make_move_iterator(pending_out.begin() + out_cursor),
            std::make_move_iterator(pending_out.end()));
        out_cursor = pending_out.size();
        if (has_limit) {
            if (drain.size() > limit_remaining) drain.resize(limit_remaining);
            limit_remaining -= drain.size();
            if (limit_remaining == 0) upstream_done = true;
        }
        return std::optional{Chunk<Joined<L, R>>::from(std::move(drain))};
    }
};

} // namespace detail

namespace detail {
// Adapter: wrap StreamHandle<Joined<L,R>> into StreamHandle<Record<Joined<L,R>>>
// by lifting each Joined into a Record using its `key` field.
template <typename L, typename R>
struct WrapAsRecordImpl {
    using J  = Joined<L, R>;
    using RJ = Record<J>;
    StreamHandle<J> src;
    explicit WrapAsRecordImpl(StreamHandle<J> s) : src(std::move(s)) {}
    WrapAsRecordImpl(const WrapAsRecordImpl& o) : src(o.src.clone()) {}

    [[nodiscard]] auto pull() -> Result<std::optional<Chunk<RJ>>> {
        auto r = src.pull();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) return std::optional<Chunk<RJ>>{};
        std::vector<RJ> out;
        out.reserve(r->value().size());
        for (auto& j : r->value()) {
            RJ rj{ .key = j.key, .value = std::move(j) };
            out.push_back(std::move(rj));
        }
        return std::optional{Chunk<RJ>::from(std::move(out))};
    }
};

template <typename L, typename R>
[[nodiscard]] inline auto wrap_as_record(StreamHandle<Joined<L, R>> src)
    -> StreamHandle<Record<Joined<L, R>>>
{
    auto* impl = new WrapAsRecordImpl<L, R>(std::move(src));
    return make_stream_handle<Record<Joined<L, R>>>(impl);
}
} // namespace detail

// ── Public join API ──
//
// Extractor has signature: Result<std::string>(const Record<L>&)
// It returns the **encoded right-side key bytes** for the given left record.

template <typename L, typename R, typename Extractor>
[[nodiscard]] auto join(RecordStream<L> left,
                        StoreRef<R> right,
                        Extractor extractor,
                        JoinOptions opts = {})
    -> Result<RecordStream<Joined<L, R>>>
{
    PlannerInputs inputs{
        .left_caps  = StoreCapabilities{},     // streams are inherently Computed
        .right_caps = right.capabilities(),
        .left_cost  = CostHint{},
        .right_cost = right.cost_hint(),
        .opts       = opts,
    };
    inputs.left_caps.cost_tier = CostTier::Computed;
    inputs.right_caps.cost_tier = CostTier::Local;
    if (right.handle() == nullptr) {
        return std::unexpected(Error{"Join", "right StoreRef is null"});
    }

    auto plan = JoinPlanner::plan(inputs);
    auto src  = std::move(left).take_handle();

    const auto strat = plan.strategy;

    if (strat == JoinStrategy::BatchIndexNestedLoop
        || strat == JoinStrategy::IndexNestedLoop)
    {
        const std::size_t bs = strat == JoinStrategy::IndexNestedLoop
                                  ? std::size_t{1}
                                  : plan.batch_size;
        auto* impl = new detail::BatchIndexNLImpl<L, R, Extractor>(
            std::move(src), std::move(right), std::move(extractor),
            opts.kind, bs, opts.limit);
        return RecordStream<Joined<L, R>>{
            detail::wrap_as_record<L, R>(
                make_stream_handle<Joined<L, R>>(impl))};
    }

    if (strat == JoinStrategy::HashJoin) {
        // Build right index by scanning the entire right side.
        auto rs = right.stream_all();
        if (!rs) return std::unexpected(rs.error());
        detail::DenseMap<Key, R> idx;
        auto& s = *rs;
        while (true) {
            auto pr = s.pull();
            if (!pr) return std::unexpected(pr.error());
            if (!pr->has_value()) break;
            for (auto& rec : pr->value()) {
                Key k{rec.key};
                idx.try_emplace(std::move(k), rec.value);
                if (opts.hash_build_cap > 0
                    && idx.size() >= opts.hash_build_cap) {
                    return std::unexpected(Error{"HashJoinBuild",
                        "hash_build_cap exceeded"});
                }
            }
        }
        auto* impl = new detail::HashJoinImpl<L, R, Extractor>(
            std::move(src), std::move(idx), std::move(extractor),
            opts.kind, opts.limit);
        return RecordStream<Joined<L, R>>{
            detail::wrap_as_record<L, R>(
                make_stream_handle<Joined<L, R>>(impl))};
    }

    // MergeJoin / NestedLoop / default fallback: materialize right side, probe.
    auto rs = right.stream_all();
    if (!rs) return std::unexpected(rs.error());
    std::vector<Record<R>> right_all;
    auto& s = *rs;
    while (true) {
        auto pr = s.pull();
        if (!pr) return std::unexpected(pr.error());
        if (!pr->has_value()) break;
        for (auto& rec : pr->value()) right_all.push_back(rec);
    }
    auto* impl = new detail::NestedLoopImpl<L, R, Extractor>(
        std::move(src), std::move(right_all), std::move(extractor),
        opts.kind, opts.limit);
    return RecordStream<Joined<L, R>>{
        detail::wrap_as_record<L, R>(
            make_stream_handle<Joined<L, R>>(impl))};
}

} // namespace celer::materialization

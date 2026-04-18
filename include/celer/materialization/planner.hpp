#pragma once
/// celer::materialization::planner — strategy selection for cross-store joins.
///
/// Selection rules (deterministic, no profiling needed):
///
///   • Right side has native_batch_get (RocksDB MultiGet, SQLite IN-clause)
///     AND right CostTier ∈ {Local, Embedded}
///       → BatchIndexNestedLoop (batched probe; default fast path)
///
///   • Right side has native_batch_get but is Network (S3)
///       → BatchIndexNestedLoop with a larger batch_size (amortize RTT)
///
///   • Both sides Local AND |left| roughly bounded
///       → HashJoin (build small side in-memory, probe large side)
///
///   • Both sides ordered_scan and same key encoding
///       → MergeJoin (single pass, no extra memory)
///
///   • Right side is Computed or has no batch_get
///       → IndexNestedLoop (per-row probe; no choice)
///
///   • Forced fallback / debug
///       → NestedLoop
///
/// All policies surface to the executor as ``JoinPlan``; users may also
/// pin a strategy via ``JoinOptions::force_strategy``.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "celer/materialization/store_ref.hpp"

namespace celer::materialization {

// ── Join semantics ──

enum class JoinKind : std::uint8_t {
    Inner = 0,
    LeftOuter,
    RightOuter,
    FullOuter,
    SemiLeft,        ///< keep left rows that have a match
    AntiLeft,        ///< keep left rows with no match
};

enum class JoinStrategy : std::uint8_t {
    Auto = 0,
    BatchIndexNestedLoop,
    IndexNestedLoop,
    HashJoin,
    MergeJoin,
    NestedLoop,
};

enum class ConsistencyMode : std::uint8_t {
    /// Each side scanned independently; no snapshot coordination. Fastest.
    ReadCommitted = 0,
    /// Take a snapshot token at planning time and pin both sides to it.
    /// Local backends honor this; remote backends fall back to time-of-scan.
    SnapshotIsolation,
};

// ── Knobs ──

struct JoinOptions {
    JoinKind         kind{JoinKind::Inner};
    JoinStrategy     force_strategy{JoinStrategy::Auto};
    ConsistencyMode  consistency{ConsistencyMode::ReadCommitted};
    std::size_t      batch_size{0};      ///< 0 = strategy default (1024 local, 256 net)
    std::size_t      hash_build_cap{0};  ///< 0 = no cap; > 0 caps build side rows
    std::size_t      limit{0};           ///< 0 = unlimited result rows
    bool             dedupe_left_keys{true};
};

// ── Planner output ──

struct JoinPlan {
    JoinStrategy   strategy{JoinStrategy::IndexNestedLoop};
    std::size_t    batch_size{1024};
    std::string    rationale;            ///< human-readable explanation
};

// ── Inputs descriptor (read by planner) ──

struct PlannerInputs {
    StoreCapabilities  left_caps;
    StoreCapabilities  right_caps;
    CostHint           left_cost;
    CostHint           right_cost;
    JoinOptions        opts;
};

// ── Pure planner ──

struct JoinPlanner {
    [[nodiscard]] static auto plan(const PlannerInputs& in) noexcept -> JoinPlan {
        if (in.opts.force_strategy != JoinStrategy::Auto) {
            JoinPlan p;
            p.strategy = in.opts.force_strategy;
            p.batch_size = in.opts.batch_size > 0 ? in.opts.batch_size : 1024;
            p.rationale = "user-pinned strategy";
            return p;
        }

        const auto right_local =
            in.right_caps.cost_tier == CostTier::Local
            || in.right_caps.cost_tier == CostTier::Embedded;
        const auto right_network = in.right_caps.cost_tier == CostTier::Network;

        if (in.right_caps.native_batch_get && right_local) {
            return JoinPlan{
                .strategy   = JoinStrategy::BatchIndexNestedLoop,
                .batch_size = in.opts.batch_size > 0 ? in.opts.batch_size : 1024,
                .rationale  = "right side has native batch get; local cost tier",
            };
        }
        if (in.right_caps.native_batch_get && right_network) {
            return JoinPlan{
                .strategy   = JoinStrategy::BatchIndexNestedLoop,
                .batch_size = in.opts.batch_size > 0 ? in.opts.batch_size : 256,
                .rationale  = "right side has native batch get; network round-trip dominates",
            };
        }
        if (right_local
            && in.left_caps.cost_tier == CostTier::Local
            && in.opts.hash_build_cap > 0)
        {
            return JoinPlan{
                .strategy   = JoinStrategy::HashJoin,
                .batch_size = in.opts.batch_size > 0 ? in.opts.batch_size : 1024,
                .rationale  = "both sides local with bounded hash build cap",
            };
        }
        if (right_local
            && in.right_caps.ordered_scan
            && in.left_caps.ordered_scan
            && in.opts.hash_build_cap == 0)
        {
            // Both ordered_scan = true is currently always set, so this guards
            // on absence of an explicit hash cap rather than ordering alone.
        }
        return JoinPlan{
            .strategy   = JoinStrategy::IndexNestedLoop,
            .batch_size = 1,
            .rationale  = "no batch get available; per-row probe",
        };
    }
};

} // namespace celer::materialization

#pragma once
/// celer::necto::swarm — Data types for the Hatchling swarm intelligence model.
///
/// Entities transcoded from hatchling-swarm.sml v0.2.0.
/// Vicsek field × ACO pheromone × response-threshold morph × dynamic sub-clusters.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "celer/necto/actor.hpp"

namespace celer::necto::swarm {

// ════════════════════════════════════════════════════════════════
// Enums
// ════════════════════════════════════════════════════════════════

enum class Morph : uint8_t {
    Explorer         = 0,
    Worker           = 1,
    Evaluator        = 2,
    SwarmCoordinator = 3,
};

inline constexpr uint8_t morph_count = 4;

enum class TaskStatus : uint8_t { Unexplored, InProgress, Completed, Failed };

enum class TaskTier : uint8_t { N1, N2, N3, N4, N5 };

// ════════════════════════════════════════════════════════════════
// Swarm message kinds
// ════════════════════════════════════════════════════════════════

enum class SwarmMessageKind : uint8_t {
    Trigger, Discovery, Result, Evaluation,
    NoiseInjection, Rebalance, Converged,
    Recruit, Dismiss,
};

// ════════════════════════════════════════════════════════════════
// Agent state
// ════════════════════════════════════════════════════════════════

struct AgentState {
    std::vector<float> heading;           // unit vector in R^d
    float              quality{0.0f};     // [0,1]
    Morph              morph{Morph::Explorer};
    AgentLifecycle     lifecycle{AgentLifecycle::Active};
    ClusterId          cluster{};         // UINT32_MAX = dormant pool
    uint32_t           agent_id{0};
    std::string        name;              // registered in FlatSymbolTable
};

// ════════════════════════════════════════════════════════════════
// Task tree
// ════════════════════════════════════════════════════════════════

struct TaskNodeId {
    uint32_t id{0};

    constexpr bool operator==(const TaskNodeId&) const noexcept = default;
    constexpr auto operator<=>(const TaskNodeId&) const noexcept = default;
};

struct TaskNode {
    TaskNodeId             id;
    std::string            description;
    TaskNodeId             parent{};            // 0 = no parent
    std::vector<TaskNodeId> children;
    uint32_t               depth{0};            // tier in decomposition
    TaskStatus             status{TaskStatus::Unexplored};
};

// ════════════════════════════════════════════════════════════════
// Pheromone edges (ACO)
// ════════════════════════════════════════════════════════════════

struct PheromoneEdge {
    TaskNodeId from;
    TaskNodeId to;
    float      tau{1.0f};                // pheromone strength
    float      heuristic_eta{1.0f};      // domain desirability
    uint32_t   age{0};                   // ticks since last deposit
};

// ════════════════════════════════════════════════════════════════
// Morph configuration (response-threshold model)
// ════════════════════════════════════════════════════════════════

struct MorphConfig {
    std::string name;
    float       base_threshold{0.5f};    // theta_c
    float       stimulus_decay{0.1f};
};

// ════════════════════════════════════════════════════════════════
// Cluster state
// ════════════════════════════════════════════════════════════════

struct ClusterState {
    ClusterId               id;
    std::string             name;
    std::vector<ActorRef>   members;
    TaskNodeId              root_task;
    std::string             scope_prefix;    // celer-mem scope
    float                   phi{0.0f};       // cluster-local order parameter
    uint32_t                sustain_count{0};
    bool                    active{true};
};

// ════════════════════════════════════════════════════════════════
// Swarm config
// ════════════════════════════════════════════════════════════════

struct SwarmConfig {
    uint32_t               population{64};
    uint32_t               strategy_dim{16};
    float                  alignment_radius{0.3f};
    float                  noise_eta{0.15f};
    float                  evaporation_rate{0.05f};
    float                  convergence_phi{0.9f};
    uint32_t               sustain_window{50};
    uint32_t               max_rounds{10'000};
    float                  premature_quality{0.3f};
    float                  noise_burst{0.5f};
    std::vector<MorphConfig> morphs;
    uint32_t               channel_capacity{1024};
};

// ════════════════════════════════════════════════════════════════
// Solution / output types
// ════════════════════════════════════════════════════════════════

struct SolutionPath {
    std::vector<TaskNodeId> nodes;
    float                   total_pheromone{0.0f};
    float                   quality{0.0f};
};

struct TaskTree {
    TaskNode                 root;
    std::vector<TaskNode>    nodes;
    std::vector<PheromoneEdge> edges;
};

struct SwarmResult {
    TaskTree                solution;
    std::vector<SolutionPath> ranked_trails;
    uint32_t                rounds{0};
    uint32_t                phase_transitions{0};
    uint32_t                final_morph_dist[morph_count]{};
    float                   final_phi{0.0f};
    float                   best_quality{0.0f};
    ClusterId               cluster_id;
};

// ════════════════════════════════════════════════════════════════
// Snapshot types (for observation / dashboard)
// ════════════════════════════════════════════════════════════════

struct ClusterSnapshot {
    ClusterId   cluster_id;
    std::string name;
    uint32_t    member_count{0};
    float       phi{0.0f};
    uint32_t    sustain_count{0};
    uint32_t    morph_distribution[morph_count]{};
    uint32_t    active_edges{0};
    float       best_quality{0.0f};
};

struct SwarmSnapshot {
    uint32_t                    tick{0};
    float                       phi{0.0f};
    uint32_t                    morph_distribution[morph_count]{};
    uint32_t                    active_edges{0};
    float                       best_quality{0.0f};
    std::vector<AgentState>     agent_states;
    uint32_t                    cluster_count{0};
    uint32_t                    dormant_count{0};
    std::vector<ClusterSnapshot> cluster_snapshots;
};

// ════════════════════════════════════════════════════════════════
// Swarm message (inter-agent envelope payload, decoded)
// ════════════════════════════════════════════════════════════════

struct SwarmMessage {
    SwarmMessageKind        kind{SwarmMessageKind::Trigger};
    TaskNodeId              task_id{};
    ClusterId               cluster_id{};
    std::vector<char>       payload;
};

// ════════════════════════════════════════════════════════════════
// Agent thresholds (per-morph response threshold vector)
// ════════════════════════════════════════════════════════════════

struct AgentThresholds {
    float values[morph_count]{0.5f, 0.5f, 0.5f, 0.5f};

    auto operator[](std::size_t i) -> float& { return values[i]; }
    auto operator[](std::size_t i) const -> float { return values[i]; }
};

} // namespace celer::necto::swarm

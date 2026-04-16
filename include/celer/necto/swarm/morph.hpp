#pragma once
/// celer::necto::swarm::MorphScheduler — Response-threshold morph assignment (Bonabeau 1996).
///
/// Each agent has per-morph thresholds. Each morph has a global stimulus.
/// Probability of switching to morph c:
///     P(c) = s_c^n / (s_c^n + theta_c^n)    (sigmoidal response)
/// where s_c = stimulus for morph c, theta_c = agent's threshold, n = steepness.
///
/// Cluster-scoped: evaluate_cluster() operates on cluster members only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "celer/necto/swarm/types.hpp"

namespace celer::necto::swarm {

class MorphScheduler {
    std::vector<Morph>            assignments_;    // per-agent morph
    std::vector<AgentThresholds>  thresholds_;     // per-agent threshold vector
    float                         steepness_;      // sigmoid steepness (n)

public:
    MorphScheduler() = default;

    MorphScheduler(uint32_t population, const std::vector<MorphConfig>& configs,
                   float steepness)
        : steepness_(steepness) {
        assignments_.resize(population, Morph::Explorer);
        thresholds_.resize(population);

        for (auto& t : thresholds_) {
            for (uint8_t c = 0; c < morph_count && c < configs.size(); ++c) {
                t[c] = configs[c].base_threshold;
            }
        }
    }

    // ── Stimulus computation ──

    static auto compute_stimulus(Morph morph, const PheromoneGraph& graph,
                                 float cluster_phi) -> float {
        auto total = graph.node_count();
        if (total == 0) return 0.5f;

        switch (morph) {
        case Morph::Explorer: {
            auto unexplored = graph.count_by_status(TaskStatus::Unexplored);
            return static_cast<float>(unexplored) / static_cast<float>(total);
        }
        case Morph::Worker: {
            auto pending = graph.count_by_status(TaskStatus::InProgress);
            return static_cast<float>(pending) / static_cast<float>(total);
        }
        case Morph::Evaluator: {
            auto completed = graph.count_by_status(TaskStatus::Completed);
            return completed == 0 ? 0.2f
                : static_cast<float>(completed) / static_cast<float>(total);
        }
        case Morph::SwarmCoordinator:
            return 1.0f - cluster_phi;
        }
        return 0.5f;
    }

    // ── Response threshold probability ──

    static auto response_prob(float stimulus, float threshold,
                              float steepness) -> float {
        if (threshold <= 0.0f) return 1.0f;
        float sn = std::pow(stimulus, steepness);
        float tn = std::pow(threshold, steepness);
        return sn / (sn + tn);
    }

    // ── Evaluate cluster: recompute morph assignments ──

    void evaluate_cluster(const SwarmField& field, const PheromoneGraph& graph,
                          const ClusterState& cluster) {
        float phi = field.order_parameter_cluster(cluster);

        float stimuli[morph_count];
        for (uint8_t c = 0; c < morph_count; ++c) {
            stimuli[c] = compute_stimulus(static_cast<Morph>(c), graph, phi);
        }

        for (auto ref : cluster.members) {
            auto i = ref.index;
            if (i >= assignments_.size()) continue;
            if (field[i].lifecycle == AgentLifecycle::Dormant) continue;

            float best_prob = -1.0f;
            Morph best_morph = Morph::Explorer;

            for (uint8_t c = 0; c < morph_count; ++c) {
                float p = response_prob(stimuli[c], thresholds_[i][c], steepness_);
                if (p > best_prob) {
                    best_prob = p;
                    best_morph = static_cast<Morph>(c);
                }
            }
            assignments_[i] = best_morph;
        }
    }

    // ── Distribution queries ──

    [[nodiscard]] auto distribution() const -> std::array<uint32_t, morph_count> {
        std::array<uint32_t, morph_count> counts{};
        for (auto m : assignments_) counts[static_cast<uint8_t>(m)]++;
        return counts;
    }

    [[nodiscard]] auto distribution_cluster(const ClusterState& cluster) const
        -> std::array<uint32_t, morph_count> {
        std::array<uint32_t, morph_count> counts{};
        for (auto ref : cluster.members) {
            if (ref.index < assignments_.size()) {
                counts[static_cast<uint8_t>(assignments_[ref.index])]++;
            }
        }
        return counts;
    }

    [[nodiscard]] auto morph_of(uint32_t agent_id) const -> Morph {
        if (agent_id >= assignments_.size()) return Morph::Explorer;
        return assignments_[agent_id];
    }

    // ── Accessors ──

    auto assignments() -> std::vector<Morph>& { return assignments_; }
    auto assignments() const -> const std::vector<Morph>& { return assignments_; }
    auto thresholds() -> std::vector<AgentThresholds>& { return thresholds_; }
    auto thresholds() const -> const std::vector<AgentThresholds>& { return thresholds_; }
    [[nodiscard]] auto population() const -> uint32_t {
        return static_cast<uint32_t>(assignments_.size());
    }
};

} // namespace celer::necto::swarm

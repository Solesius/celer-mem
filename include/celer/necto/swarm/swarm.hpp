#pragma once
/// celer::necto::swarm::Swarm — Top-level swarm coordinator.
///
/// Owns: SwarmField, MorphScheduler, ClusterService, ActorSystem ref.
/// Implements the tick DAG from hatchling-swarm.sml:
///   1. morph evaluation (per-cluster)
///   2. stream pull + dispatch (per-active-agent)
///   3. pheromone evaporation
///   4. Vicsek alignment
///   5. convergence check + premature convergence escape
///
/// All agents start dormant. Clusters recruit from the dormant pool.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "celer/necto/actor.hpp"
#include "celer/necto/swarm/cluster.hpp"
#include "celer/necto/swarm/field.hpp"
#include "celer/necto/swarm/morph.hpp"
#include "celer/necto/swarm/pheromone.hpp"
#include "celer/necto/swarm/types.hpp"

namespace celer::necto::swarm {

class Swarm {
    SwarmConfig       cfg_;
    SwarmField        field_;
    MorphScheduler    scheduler_;
    ClusterService    clusters_;
    ActorSystem*      system_;       // non-owning — system lifetime managed externally
    uint32_t          tick_count_{0};
    uint32_t          phase_transitions_{0};

public:
    Swarm() : system_(nullptr) {}

    Swarm(SwarmConfig cfg, ActorSystem& system)
        : cfg_(std::move(cfg))
        , field_(cfg_.population, cfg_.strategy_dim, cfg_.alignment_radius, cfg_.noise_eta)
        , scheduler_(cfg_.population, cfg_.morphs, 2.0f)
        , system_(&system) {}

    // ── Tick single cluster (the 5-phase DAG) ──

    void tick_cluster(ClusterState& cluster) {
        if (!cluster.active || !system_) return;

        auto* graph = clusters_.graph_for(cluster);
        if (!graph) return;

        // Phase 1: morph evaluation
        scheduler_.evaluate_cluster(field_, *graph, cluster);

        // Phase 2: stream pull + dispatch (via actor system tick)
        for (auto ref : cluster.members) {
            if (ref.index >= field_.population()) continue;
            if (field_[ref.index].lifecycle == AgentLifecycle::Dormant) continue;
            system_->tick(ref);
        }

        // Phase 3: pheromone evaporation
        graph->evaporate();

        // Phase 4: Vicsek alignment
        field_.align_cluster(cluster);

        // Phase 5: update cluster phi
        cluster.phi = field_.order_parameter_cluster(cluster);
    }

    // ── Tick all active clusters ──

    void tick_all_clusters() {
        auto active = clusters_.active_clusters_mut();
        for (auto* cluster : active) {
            tick_cluster(*cluster);
            check_convergence(*cluster);
        }
        ++tick_count_;
    }

    // ── Run cluster to convergence ──

    auto run_cluster_to_convergence(ClusterState& cluster) -> SwarmResult {
        bool prev_above = false;
        uint32_t local_transitions = 0;

        for (uint32_t round = 0; round < cfg_.max_rounds; ++round) {
            tick_cluster(cluster);

            float avg_q = mean_quality_cluster(cluster);
            bool above = cluster.phi > cfg_.convergence_phi;

            if (above && avg_q > cfg_.premature_quality) {
                cluster.sustain_count++;
                if (cluster.sustain_count >= cfg_.sustain_window) {
                    return build_result(cluster, round, local_transitions);
                }
            } else if (above && avg_q <= cfg_.premature_quality) {
                field_.inject_noise_cluster(cluster, cfg_.noise_burst);
                cluster.sustain_count = 0;
            } else {
                if (prev_above) ++local_transitions;
                cluster.sustain_count = 0;
            }
            prev_above = above;
        }

        return build_result(cluster, cfg_.max_rounds, local_transitions);
    }

    // ── Snapshot ──

    [[nodiscard]] auto snapshot() const -> SwarmSnapshot {
        SwarmSnapshot snap;
        snap.tick           = tick_count_;
        snap.phi            = field_.order_parameter_global();
        snap.agent_states   = field_.agents();

        auto dist = scheduler_.distribution();
        for (uint8_t c = 0; c < morph_count; ++c) snap.morph_distribution[c] = dist[c];

        auto active = clusters_.active_clusters();
        snap.cluster_count  = static_cast<uint32_t>(active.size());
        snap.dormant_count  = clusters_.dormant_count(field_.agents());

        for (const auto* c : active) {
            const auto* graph = clusters_.graph_for(*c);
            auto cdist = scheduler_.distribution_cluster(*c);

            ClusterSnapshot cs;
            cs.cluster_id    = c->id;
            cs.name          = c->name;
            cs.member_count  = static_cast<uint32_t>(c->members.size());
            cs.phi           = c->phi;
            cs.sustain_count = c->sustain_count;
            for (uint8_t m = 0; m < morph_count; ++m) cs.morph_distribution[m] = cdist[m];
            cs.active_edges  = graph ? graph->edge_count() : 0;
            cs.best_quality  = best_quality_cluster(*c);
            snap.cluster_snapshots.push_back(std::move(cs));
            snap.active_edges += cs.active_edges;
        }

        snap.best_quality = 0.0f;
        for (const auto& cs : snap.cluster_snapshots) {
            snap.best_quality = std::max(snap.best_quality, cs.best_quality);
        }

        return snap;
    }

    // ── Accessors ──

    auto config() -> SwarmConfig& { return cfg_; }
    auto config() const -> const SwarmConfig& { return cfg_; }
    auto field() -> SwarmField& { return field_; }
    auto field() const -> const SwarmField& { return field_; }
    auto scheduler() -> MorphScheduler& { return scheduler_; }
    auto scheduler() const -> const MorphScheduler& { return scheduler_; }
    auto cluster_service() -> ClusterService& { return clusters_; }
    auto cluster_service() const -> const ClusterService& { return clusters_; }
    auto system() -> ActorSystem* { return system_; }
    [[nodiscard]] auto tick_count() const noexcept -> uint32_t { return tick_count_; }

private:
    [[nodiscard]] auto mean_quality_cluster(const ClusterState& cluster) const -> float {
        if (cluster.members.empty()) return 0.0f;
        float sum = 0.0f;
        for (auto ref : cluster.members) {
            if (ref.index < field_.population())
                sum += field_[ref.index].quality;
        }
        return sum / static_cast<float>(cluster.members.size());
    }

    [[nodiscard]] auto best_quality_cluster(const ClusterState& cluster) const -> float {
        float best = 0.0f;
        for (auto ref : cluster.members) {
            if (ref.index < field_.population())
                best = std::max(best, field_[ref.index].quality);
        }
        return best;
    }

    void check_convergence(ClusterState& cluster) {
        float avg_q = mean_quality_cluster(cluster);
        bool above = cluster.phi > cfg_.convergence_phi;

        if (above && avg_q <= cfg_.premature_quality) {
            field_.inject_noise_cluster(cluster, cfg_.noise_burst);
            cluster.sustain_count = 0;
        } else if (above && avg_q > cfg_.premature_quality) {
            cluster.sustain_count++;
        } else {
            cluster.sustain_count = 0;
        }
    }

    auto build_result(const ClusterState& cluster, uint32_t rounds,
                      uint32_t transitions) const -> SwarmResult {
        SwarmResult result;
        const auto* graph = clusters_.graph_for(cluster);
        if (graph) result.solution = graph->task_tree();
        result.rounds            = rounds;
        result.phase_transitions = transitions;
        auto dist = scheduler_.distribution_cluster(cluster);
        for (uint8_t c = 0; c < morph_count; ++c)
            result.final_morph_dist[c] = dist[c];
        result.final_phi    = cluster.phi;
        result.best_quality = best_quality_cluster(cluster);
        result.cluster_id   = cluster.id;
        return result;
    }
};

} // namespace celer::necto::swarm

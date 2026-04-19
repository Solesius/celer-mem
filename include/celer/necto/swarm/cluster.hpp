#pragma once
/// celer::necto::swarm::ClusterService — Dynamic sub-cluster management.
///
/// Clusters are named groups of agents scoped to a root task.
/// Agents in the dormant pool have lifecycle=Dormant and cluster=UINT32_MAX.
///
/// Operations: create, recruit, dismiss, dissolve, merge, transfer.
/// Each cluster owns a PheromoneGraph (cluster-scoped pheromone isolation).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "celer/core/symbol_table.hpp"
#include "celer/necto/actor.hpp"
#include "celer/necto/swarm/field.hpp"
#include "celer/necto/swarm/pheromone.hpp"
#include "celer/necto/swarm/types.hpp"

namespace celer::necto::swarm {

class ClusterService {
    std::vector<ClusterState>   clusters_;
    std::vector<PheromoneGraph> graphs_;         // parallel to clusters_
    std::vector<std::string>    cluster_names_;  // for symbol table rebuild
    FlatSymbolTable             names_;
    bool                        names_dirty_{true};
    uint32_t                    next_id_{0};

    void rebuild_names() {
        if (!names_dirty_) return;
        std::vector<std::size_t> values;
        values.reserve(cluster_names_.size());
        for (std::size_t i = 0; i < cluster_names_.size(); ++i)
            values.push_back(i);
        names_ = FlatSymbolTable::build(cluster_names_, values);
        names_dirty_ = false;
    }

public:
    ClusterService() = default;

    // ── Create a new cluster ──

    auto create_cluster(std::string name, TaskNode root_task,
                        float rho, float Q, float alpha, float beta,
                        uint64_t seed = 42)
        -> ClusterState& {
        ClusterId cid{next_id_++};
        std::string scope = "clusters/" + name;
        ClusterState cs;
        cs.id           = cid;
        cs.name         = name;
        cs.root_task    = root_task.id;
        cs.scope_prefix = std::move(scope);

        PheromoneGraph graph(rho, Q, alpha, beta, seed);
        graph.add_node(std::move(root_task));

        clusters_.push_back(std::move(cs));
        graphs_.push_back(std::move(graph));
        cluster_names_.push_back(name);
        names_dirty_ = true;

        return clusters_.back();
    }

    // ── Recruit dormant agents into a cluster ──

    auto recruit(ActorSystem& system, SwarmField& field,
                 ClusterState& cluster,
                 const std::vector<AgentState>& agents,
                 uint32_t count) -> std::vector<ActorRef> {
        std::vector<ActorRef> recruited;
        for (const auto& a : agents) {
            if (recruited.size() >= count) break;
            if (a.lifecycle != AgentLifecycle::Dormant) continue;

            ActorRef ref{a.agent_id};
            system.set_lifecycle(ref, AgentLifecycle::Active);
            system.set_cluster(ref, cluster.id);
            cluster.members.push_back(ref);
            field[a.agent_id].lifecycle = AgentLifecycle::Active;
            field[a.agent_id].cluster   = cluster.id;
            field.randomize_headings({ref});
            recruited.push_back(ref);
        }
        return recruited;
    }

    // ── Recruit by quality (best-first) ──

    auto recruit_by_quality(ActorSystem& system, SwarmField& field,
                            ClusterState& cluster,
                            std::vector<AgentState> agents,
                            uint32_t count) -> std::vector<ActorRef> {
        std::sort(agents.begin(), agents.end(),
                  [](const AgentState& a, const AgentState& b) {
                      return a.quality > b.quality;
                  });
        return recruit(system, field, cluster, agents, count);
    }

    // ── Dismiss agents back to dormant pool ──

    void dismiss(ActorSystem& system, SwarmField& field,
                 ClusterState& cluster,
                 const std::vector<ActorRef>& refs) {
        for (auto ref : refs) {
            system.set_lifecycle(ref, AgentLifecycle::Dormant);
            system.set_cluster(ref, ClusterId{});
            system.channel_drain(ref);
            if (ref.index < field.population()) {
                field[ref.index].lifecycle = AgentLifecycle::Dormant;
                field[ref.index].cluster   = ClusterId{};
            }
            std::erase_if(cluster.members, [ref](ActorRef r) { return r == ref; });
        }
    }

    // ── Dismiss all members ──

    void dismiss_all(ActorSystem& system, SwarmField& field,
                     ClusterState& cluster) {
        auto members = cluster.members;  // copy — dismiss modifies members
        dismiss(system, field, cluster, members);
    }

    // ── Dissolve cluster (dismiss all + deactivate) ──

    void dissolve(ActorSystem& system, SwarmField& field,
                  ClusterState& cluster) {
        dismiss_all(system, field, cluster);
        cluster.active = false;
    }

    // ── Merge source into target ──

    void merge(ActorSystem& system, SwarmField& field,
               ClusterState& source, ClusterState& target) {
        for (auto ref : source.members) {
            system.set_cluster(ref, target.id);
            if (ref.index < field.population()) {
                field[ref.index].cluster = target.id;
            }
            target.members.push_back(ref);
        }
        source.members.clear();
        source.active = false;
    }

    // ── Transfer agents between clusters ──

    void transfer(ActorSystem& system, SwarmField& field,
                  ClusterState& source, ClusterState& target,
                  const std::vector<ActorRef>& refs) {
        for (auto ref : refs) {
            std::erase_if(source.members, [ref](ActorRef r) { return r == ref; });
            target.members.push_back(ref);
            system.set_cluster(ref, target.id);
            system.channel_drain(ref);
            if (ref.index < field.population()) {
                field[ref.index].cluster = target.id;
            }
        }
    }

    // ── Queries ──

    [[nodiscard]] auto dormant_count(const std::vector<AgentState>& agents) const -> uint32_t {
        uint32_t c = 0;
        for (const auto& a : agents) {
            if (a.lifecycle == AgentLifecycle::Dormant) ++c;
        }
        return c;
    }

    [[nodiscard]] auto active_clusters() const -> std::vector<const ClusterState*> {
        std::vector<const ClusterState*> result;
        for (const auto& c : clusters_) {
            if (c.active) result.push_back(&c);
        }
        return result;
    }

    auto active_clusters_mut() -> std::vector<ClusterState*> {
        std::vector<ClusterState*> result;
        for (auto& c : clusters_) {
            if (c.active) result.push_back(&c);
        }
        return result;
    }

    auto resolve(std::string_view name) -> ClusterState* {
        rebuild_names();
        auto idx = names_.find(name);
        if (!idx || *idx >= clusters_.size()) return nullptr;
        return &clusters_[*idx];
    }

    auto graph_for(const ClusterState& cluster) -> PheromoneGraph* {
        auto idx = cluster.id.index;
        if (idx >= graphs_.size()) return nullptr;
        return &graphs_[idx];
    }

    auto graph_for(const ClusterState& cluster) const -> const PheromoneGraph* {
        auto idx = cluster.id.index;
        if (idx >= graphs_.size()) return nullptr;
        return &graphs_[idx];
    }

    [[nodiscard]] auto cluster_count() const -> uint32_t {
        return static_cast<uint32_t>(clusters_.size());
    }
};

} // namespace celer::necto::swarm

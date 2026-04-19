#pragma once
/// celer::necto::swarm::PheromoneGraph — ACO pheromone model (Dorigo 1996).
///
/// In-memory pheromone graph over TaskNode / PheromoneEdge.
/// Cluster-scoped: each cluster gets its own PheromoneGraph instance.
///
/// Operations: add_node, deposit, evaporate, select_next (roulette), prune.
/// Edge storage: flat vector (edges typically < 1000 per cluster).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "celer/necto/swarm/types.hpp"

namespace celer::necto::swarm {

class PheromoneGraph {
    std::vector<TaskNode>      nodes_;
    std::vector<PheromoneEdge> edges_;
    float                      rho_;        // evaporation rate
    float                      Q_;          // deposit scaling
    float                      alpha_;      // pheromone weight in selection
    float                      beta_;       // heuristic weight in selection
    std::mt19937               rng_;

public:
    PheromoneGraph() = default;

    PheromoneGraph(float rho, float Q, float alpha, float beta,
                   uint64_t seed = 42)
        : rho_(rho), Q_(Q), alpha_(alpha), beta_(beta), rng_(seed) {}

    // ── Node management ──

    void add_node(TaskNode node) {
        nodes_.push_back(std::move(node));
    }

    [[nodiscard]] auto find_node(TaskNodeId id) const -> const TaskNode* {
        for (const auto& n : nodes_) {
            if (n.id == id) return &n;
        }
        return nullptr;
    }

    auto find_node_mut(TaskNodeId id) -> TaskNode* {
        for (auto& n : nodes_) {
            if (n.id == id) return &n;
        }
        return nullptr;
    }

    // ── Edge management ──

    auto find_edge(TaskNodeId from, TaskNodeId to) -> PheromoneEdge* {
        for (auto& e : edges_) {
            if (e.from == from && e.to == to) return &e;
        }
        return nullptr;
    }

    [[nodiscard]] auto find_edge(TaskNodeId from, TaskNodeId to) const -> const PheromoneEdge* {
        for (const auto& e : edges_) {
            if (e.from == from && e.to == to) return &e;
        }
        return nullptr;
    }

    auto edges_from(TaskNodeId from) const -> std::vector<PheromoneEdge> {
        std::vector<PheromoneEdge> result;
        for (const auto& e : edges_) {
            if (e.from == from) result.push_back(e);
        }
        return result;
    }

    void ensure_edge(TaskNodeId from, TaskNodeId to) {
        if (!find_edge(from, to)) {
            edges_.push_back(PheromoneEdge{from, to, 1.0f, 1.0f, 0});
        }
    }

    // ── Deposit pheromone along a path ──

    void deposit(const std::vector<TaskNodeId>& path, float quality) {
        if (path.size() < 2 || quality <= 0.0f) return;
        float delta_tau = Q_ / (1.0f / quality);
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            ensure_edge(path[i], path[i + 1]);
            auto* e = find_edge(path[i], path[i + 1]);
            if (e) {
                e->tau += delta_tau;
                e->age = 0;
            }
        }
    }

    // ── Evaporate all edges ──

    void evaporate() {
        for (auto& e : edges_) {
            e.tau *= (1.0f - rho_);
            ++e.age;
        }
    }

    // ── Roulette selection (ACO) ──

    auto select_next(TaskNodeId from,
                     const std::vector<TaskNodeId>& feasible) -> std::optional<TaskNodeId> {
        if (feasible.empty()) return std::nullopt;

        std::vector<float> weights;
        weights.reserve(feasible.size());
        for (auto to : feasible) {
            const auto* e = find_edge(from, to);
            float tau = e ? e->tau : 1.0f;
            float eta = e ? e->heuristic_eta : 1.0f;
            weights.push_back(std::pow(tau, alpha_) * std::pow(eta, beta_));
        }

        float total = 0.0f;
        for (auto w : weights) total += w;
        if (total <= 0.0f) return feasible[0];

        std::uniform_real_distribution<float> dist(0.0f, total);
        float r = dist(rng_);
        float cumulative = 0.0f;
        for (std::size_t i = 0; i < feasible.size(); ++i) {
            cumulative += weights[i];
            if (cumulative >= r) return feasible[i];
        }
        return feasible.back();
    }

    // ── Prune stale edges ──

    auto prune(float min_tau) -> uint32_t {
        auto before = edges_.size();
        std::erase_if(edges_, [min_tau](const PheromoneEdge& e) {
            return e.tau < min_tau;
        });
        return static_cast<uint32_t>(before - edges_.size());
    }

    // ── Build task tree from current state ──

    [[nodiscard]] auto task_tree() const -> TaskTree {
        TaskTree tree;
        tree.nodes = nodes_;
        tree.edges = edges_;
        for (const auto& n : nodes_) {
            if (n.depth == 0) { tree.root = n; break; }
        }
        return tree;
    }

    // ── Node status helpers ──

    auto count_by_status(TaskStatus status) const -> uint32_t {
        uint32_t c = 0;
        for (const auto& n : nodes_) {
            if (n.status == status) ++c;
        }
        return c;
    }

    void mark_status(TaskNodeId id, TaskStatus status) {
        auto* n = find_node_mut(id);
        if (n) n->status = status;
    }

    // ── Accessors ──

    auto nodes() -> std::vector<TaskNode>& { return nodes_; }
    auto nodes() const -> const std::vector<TaskNode>& { return nodes_; }
    auto edges() -> std::vector<PheromoneEdge>& { return edges_; }
    auto edges() const -> const std::vector<PheromoneEdge>& { return edges_; }
    [[nodiscard]] auto edge_count() const -> uint32_t {
        return static_cast<uint32_t>(edges_.size());
    }
    [[nodiscard]] auto node_count() const -> uint32_t {
        return static_cast<uint32_t>(nodes_.size());
    }
};

} // namespace celer::necto::swarm

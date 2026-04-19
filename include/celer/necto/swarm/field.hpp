#pragma once
/// celer::necto::swarm::SwarmField — Vicsek alignment model in R^d.
///
/// Implements the continuous Vicsek model (Vicsek et al., 1995) generalized
/// to arbitrary strategy dimensions. Each agent carries a unit-vector heading
/// in R^d. Alignment updates average neighbor headings within a cosine-distance
/// radius, then add Gaussian noise scaled by eta.
///
/// Cluster-scoped: align_cluster() operates only on members of a given cluster.
/// order_parameter_cluster() computes phi over cluster members only.
///
/// All operations are O(N^2) in cluster size (brute-force neighbor search).
/// For 64 agents this is sub-microsecond. VP-tree upgrade later if needed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include "celer/necto/swarm/types.hpp"

namespace celer::necto::swarm {

class SwarmField {
    std::vector<AgentState> agents_;
    uint32_t                dim_;
    float                   radius_;
    float                   eta_;
    std::mt19937            rng_;

    static auto dot(const std::vector<float>& a,
                    const std::vector<float>& b) -> float {
        float s = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
        return s;
    }

    static auto norm(const std::vector<float>& v) -> float {
        return std::sqrt(dot(v, v));
    }

    static void normalize(std::vector<float>& v) {
        float n = norm(v);
        if (n > 1e-12f) {
            for (auto& x : v) x /= n;
        }
    }

    static auto cosine_distance(const std::vector<float>& a,
                                const std::vector<float>& b) -> float {
        float d = dot(a, b);
        float na = norm(a);
        float nb = norm(b);
        if (na < 1e-12f || nb < 1e-12f) return 1.0f;
        return 1.0f - d / (na * nb);
    }

    auto sample_gaussian_vector(float scale) -> std::vector<float> {
        std::normal_distribution<float> dist(0.0f, scale);
        std::vector<float> v(dim_);
        for (auto& x : v) x = dist(rng_);
        return v;
    }

    auto random_unit_vector() -> std::vector<float> {
        auto v = sample_gaussian_vector(1.0f);
        normalize(v);
        return v;
    }

    static auto add_vectors(const std::vector<float>& a,
                            const std::vector<float>& b) -> std::vector<float> {
        std::vector<float> r(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) r[i] = a[i] + b[i];
        return r;
    }

    static auto zero_vector(uint32_t d) -> std::vector<float> {
        return std::vector<float>(d, 0.0f);
    }

public:
    SwarmField() = default;

    SwarmField(uint32_t population, uint32_t dim, float radius, float eta,
               uint64_t seed = 42)
        : dim_(dim), radius_(radius), eta_(eta), rng_(seed) {
        agents_.resize(population);
        for (uint32_t i = 0; i < population; ++i) {
            agents_[i].heading   = random_unit_vector();
            agents_[i].agent_id  = i;
            agents_[i].lifecycle = AgentLifecycle::Dormant;
        }
    }

    // ── Cluster-scoped alignment (Vicsek update) ──

    void align_cluster(const ClusterState& cluster) {
        for (auto ref : cluster.members) {
            if (ref.index >= agents_.size()) continue;
            auto& agent = agents_[ref.index];
            if (agent.lifecycle == AgentLifecycle::Dormant) continue;

            auto weighted_sum = zero_vector(dim_);
            uint32_t count = 0;

            for (auto nref : cluster.members) {
                if (nref.index >= agents_.size()) continue;
                const auto& neighbor = agents_[nref.index];
                if (neighbor.lifecycle == AgentLifecycle::Dormant) continue;
                if (cosine_distance(agent.heading, neighbor.heading) < radius_) {
                    for (uint32_t d = 0; d < dim_; ++d)
                        weighted_sum[d] += neighbor.heading[d];
                    ++count;
                }
            }

            if (count > 0) {
                auto noise = sample_gaussian_vector(eta_);
                agent.heading = add_vectors(weighted_sum, noise);
                normalize(agent.heading);
            }
        }
    }

    // ── Order parameter (cluster-local) ──

    [[nodiscard]] auto order_parameter_cluster(const ClusterState& cluster) const -> float {
        auto sum = zero_vector(dim_);
        uint32_t count = 0;
        for (auto ref : cluster.members) {
            if (ref.index >= agents_.size()) continue;
            const auto& a = agents_[ref.index];
            if (a.lifecycle == AgentLifecycle::Dormant) continue;
            for (uint32_t d = 0; d < dim_; ++d) sum[d] += a.heading[d];
            ++count;
        }
        if (count == 0) return 0.0f;
        return norm(sum) / static_cast<float>(count);
    }

    // ── Global order parameter (all active agents) ──

    [[nodiscard]] auto order_parameter_global() const -> float {
        auto sum = zero_vector(dim_);
        uint32_t count = 0;
        for (const auto& a : agents_) {
            if (a.lifecycle == AgentLifecycle::Dormant) continue;
            for (uint32_t d = 0; d < dim_; ++d) sum[d] += a.heading[d];
            ++count;
        }
        if (count == 0) return 0.0f;
        return norm(sum) / static_cast<float>(count);
    }

    // ── Noise injection (premature convergence escape) ──

    void inject_noise_cluster(const ClusterState& cluster, float burst_eta) {
        for (auto ref : cluster.members) {
            if (ref.index >= agents_.size()) continue;
            auto& agent = agents_[ref.index];
            if (agent.lifecycle == AgentLifecycle::Dormant) continue;
            auto noise = sample_gaussian_vector(burst_eta);
            agent.heading = add_vectors(agent.heading, noise);
            normalize(agent.heading);
        }
    }

    // ── Randomize headings for recruited agents ──

    void randomize_headings(const std::vector<ActorRef>& refs) {
        for (auto ref : refs) {
            if (ref.index < agents_.size()) {
                agents_[ref.index].heading = random_unit_vector();
            }
        }
    }

    // ── Accessors ──

    auto operator[](uint32_t i) -> AgentState& { return agents_[i]; }
    auto operator[](uint32_t i) const -> const AgentState& { return agents_[i]; }
    auto agents() -> std::vector<AgentState>& { return agents_; }
    auto agents() const -> const std::vector<AgentState>& { return agents_; }
    [[nodiscard]] auto dim() const noexcept -> uint32_t { return dim_; }
    [[nodiscard]] auto population() const -> uint32_t {
        return static_cast<uint32_t>(agents_.size());
    }

    auto eta() const noexcept -> float { return eta_; }
    void set_eta(float e) noexcept { eta_ = e; }
};

} // namespace celer::necto::swarm

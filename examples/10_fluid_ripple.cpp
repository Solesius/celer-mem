/// celer::necto::swarm — Navier-Stokes Container Flow
///
/// A swarm of agents arranged on a 2D Cartesian grid models an
/// incompressible flow field with a driven inlet jet entering an open-top
/// container. The state is a compact stable-fluids approximation of
/// the 2D incompressible Navier-Stokes system:
///
///   ∂u/∂t + (u·∇)u = -∇p + ν∇²u + f,     ∇·u = 0
///
/// Physics-to-swarm mapping:
///   Vicsek alignment  = local advection / coherent transport direction
///   η noise annealing = sub-grid turbulence / unresolved mixing
///   Pheromone trails  = wetted-path and vorticity memory through the basin
///   Oracles           = inlet forcing / jet activation
///   φ convergence     = quiescent fill state after projection and settling
///   Morphs:
///     Explorer    = inlet jet propagation
///     Worker      = wall shear / obstacle handling
///     Evaluator   = pressure projection / divergence audit
///     Coordinator = settling / recirculation damping
///
/// Usage:  ./example_10_fluid_ripple [grid_size] [jet_scale]
/// Default: 34×34 grid, jet_scale = 1.0

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "celer/necto/swarm/swarm.hpp"

using namespace celer::necto;
using namespace celer::necto::swarm;

namespace ansi {
    constexpr const char* clear      = "\033[2J\033[H";
    constexpr const char* reset      = "\033[0m";
    constexpr const char* dim        = "\033[2m";
    constexpr const char* header     = "\033[1;37m";
    constexpr const char* stat_val   = "\033[1;96m";
    constexpr const char* border     = "\033[90m";
    constexpr const char* wall       = "\033[1;90m";
    constexpr const char* nozzle     = "\033[1;33m";
    constexpr const char* water_hi   = "\033[1;37m";
    constexpr const char* water_mid  = "\033[1;36m";
    constexpr const char* water_lo   = "\033[1;34m";
    constexpr const char* water_deep = "\033[34m";
    constexpr const char* spray      = "\033[1;94m";
    constexpr const char* mist       = "\033[94m";
    constexpr const char* calm       = "\033[90m";
    constexpr const char* ok         = "\033[1;32m";
    constexpr const char* warn       = "\033[1;33m";
    constexpr const char* explorer   = "\033[36m";
    constexpr const char* worker     = "\033[33m";
    constexpr const char* evaluator  = "\033[32m";
    constexpr const char* coord      = "\033[35m";
    constexpr const char* fill_bar   = "\033[44m";
    constexpr const char* energy_bar = "\033[46m";
    constexpr const char* div_bar    = "\033[45m";
}

static auto clamp01(float x) -> float {
    return std::clamp(x, 0.0f, 1.0f);
}

struct SimParams {
    float    dt{0.12f};
    float    viscosity{0.0007f};
    float    diffusion{0.00004f};
    float    gravity{0.85f};
    float    inlet_density{0.58f};
    float    inlet_u{0.72f};
    float    inlet_v{1.85f};
    float    density_decay{0.99985f};
    uint32_t pressure_iters{24};
    uint32_t inflow_steps{260};
};

struct FlowStats {
    float kinetic{0.0f};
    float divergence_l1{0.0f};
    float fill_ratio{0.0f};
    float mass_in_basin{0.0f};
    float max_speed{0.0f};
    float mean_curl{0.0f};
};

struct FlowGrid {
    uint32_t N;
    uint32_t basin_left{0};
    uint32_t basin_right{0};
    uint32_t basin_top{0};
    uint32_t basin_floor{0};
    uint32_t nozzle_row{0};
    uint32_t nozzle_col{0};

    std::vector<float> u;
    std::vector<float> v;
    std::vector<float> u_prev;
    std::vector<float> v_prev;
    std::vector<float> density;
    std::vector<float> density_prev;
    std::vector<float> pressure;
    std::vector<float> divergence;
    std::vector<float> residence;

    std::vector<uint8_t> solid;
    std::vector<uint8_t> basin;
    std::vector<uint8_t> nozzle;

    std::vector<float> kinetic_history;
    std::vector<float> fill_history;
    std::vector<float> divergence_history;
    std::vector<float> circulation_history;

    explicit FlowGrid(uint32_t n)
        : N(n)
        , u(n * n, 0.0f)
        , v(n * n, 0.0f)
        , u_prev(n * n, 0.0f)
        , v_prev(n * n, 0.0f)
        , density(n * n, 0.0f)
        , density_prev(n * n, 0.0f)
        , pressure(n * n, 0.0f)
        , divergence(n * n, 0.0f)
        , residence(n * n, 0.0f)
        , solid(n * n, 0)
        , basin(n * n, 0)
        , nozzle(n * n, 0) {
        build_geometry();
    }

    auto idx(uint32_t r, uint32_t c) const -> uint32_t {
        return r * N + c;
    }

    auto is_solid_cell(int r, int c) const -> bool {
        if (r < 0 || c < 0 || r >= static_cast<int>(N) || c >= static_cast<int>(N)) {
            return true;
        }
        return solid[idx(static_cast<uint32_t>(r), static_cast<uint32_t>(c))] != 0;
    }

    void build_geometry() {
        basin_left = std::max(3u, N / 3);
        basin_right = std::min(N - 4, N - std::max(4u, N / 5));
        if (basin_right <= basin_left + 6) {
            basin_right = std::min(N - 3, basin_left + 6);
        }
        basin_top = std::max(4u, N / 3 + 2);
        basin_floor = N - 3;
        nozzle_row = (basin_top > 6) ? (basin_top - 6) : 2u;
        nozzle_col = (basin_left > 3) ? (basin_left - 2) : 2u;

        for (uint32_t r = 0; r < N; ++r) {
            for (uint32_t c = 0; c < N; ++c) {
                if (r == 0 || c == 0 || r == N - 1 || c == N - 1) {
                    solid[idx(r, c)] = 1;
                }
            }
        }

        for (uint32_t r = basin_top; r <= basin_floor; ++r) {
            solid[idx(r, basin_left)] = 1;
            solid[idx(r, basin_right)] = 1;
        }
        for (uint32_t c = basin_left; c <= basin_right; ++c) {
            solid[idx(basin_floor, c)] = 1;
        }
        if (basin_left > 1) {
            solid[idx(basin_top, basin_left - 1)] = 1;
        }
        if (basin_right + 1 < N) {
            solid[idx(basin_top, basin_right + 1)] = 1;
        }

        for (uint32_t r = basin_top + 1; r < basin_floor; ++r) {
            for (uint32_t c = basin_left + 1; c < basin_right; ++c) {
                basin[idx(r, c)] = 1;
            }
        }

        for (uint32_t r = nozzle_row; r < std::min(N - 1, nozzle_row + 2); ++r) {
            for (uint32_t c = nozzle_col; c < std::min(N - 1, nozzle_col + 3); ++c) {
                nozzle[idx(r, c)] = 1;
                solid[idx(r, c)] = 0;
            }
        }
        if (nozzle_col > 1) {
            solid[idx(nozzle_row, nozzle_col - 1)] = 1;
        }
        if (nozzle_row + 2 < N) {
            solid[idx(nozzle_row + 2, nozzle_col)] = 1;
            solid[idx(nozzle_row + 2, nozzle_col + 1)] = 1;
            if (nozzle_col + 2 < N) {
                solid[idx(nozzle_row + 2, nozzle_col + 2)] = 1;
            }
        }
    }

    auto neighbor_value(const std::vector<float>& field, uint32_t r, uint32_t c,
                        int nr, int nc, bool zero_on_solid) const -> float {
        if (is_solid_cell(nr, nc)) {
            return zero_on_solid ? 0.0f : field[idx(r, c)];
        }
        return field[idx(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc))];
    }

    void apply_solid_boundaries(std::vector<float>& field) const {
        for (uint32_t i = 0; i < field.size(); ++i) {
            if (solid[i]) {
                field[i] = 0.0f;
            }
        }
    }

    void clamp_density() {
        for (uint32_t i = 0; i < density.size(); ++i) {
            if (solid[i]) {
                density[i] = 0.0f;
                residence[i] = 0.0f;
            } else {
                density[i] = std::clamp(density[i], 0.0f, 1.40f);
                residence[i] = clamp01(residence[i]);
            }
        }
    }

    void add_inlet(const SimParams& params, uint32_t step) {
        if (step >= params.inflow_steps) {
            return;
        }
        float pulse = 0.88f + 0.16f * std::sin(static_cast<float>(step) * 0.12f);
        float sweep = 1.0f + 0.08f * std::sin(static_cast<float>(step) * 0.07f);
        for (uint32_t r = nozzle_row; r < std::min(N - 1, nozzle_row + 2); ++r) {
            for (uint32_t c = nozzle_col; c < std::min(N - 1, nozzle_col + 3); ++c) {
                uint32_t i = idx(r, c);
                density[i] = std::min(1.35f, density[i] + params.inlet_density * pulse);
                u[i] = params.inlet_u * pulse;
                v[i] = params.inlet_v * sweep;
                residence[i] = 1.0f;
            }
        }
    }

    void apply_forces(const SimParams& params) {
        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }
                float local_density = density[i];
                v[i] += params.gravity * params.dt * (0.15f + 0.85f * local_density);
                u[i] *= 0.9985f;
                v[i] *= 0.9985f;
            }
        }
    }

    void diffuse(std::vector<float>& field, const std::vector<float>& source,
                 float diff, float dt, uint32_t iters, bool zero_on_solid) {
        field = source;
        float a = dt * diff * static_cast<float>((N - 2) * (N - 2));
        if (a < 1e-9f) {
            apply_solid_boundaries(field);
            return;
        }

        for (uint32_t iter = 0; iter < iters; ++iter) {
            for (uint32_t r = 1; r + 1 < N; ++r) {
                for (uint32_t c = 1; c + 1 < N; ++c) {
                    uint32_t i = idx(r, c);
                    if (solid[i]) {
                        continue;
                    }
                    float sum = 0.0f;
                    sum += neighbor_value(field, r, c, static_cast<int>(r) - 1, static_cast<int>(c), zero_on_solid);
                    sum += neighbor_value(field, r, c, static_cast<int>(r) + 1, static_cast<int>(c), zero_on_solid);
                    sum += neighbor_value(field, r, c, static_cast<int>(r), static_cast<int>(c) - 1, zero_on_solid);
                    sum += neighbor_value(field, r, c, static_cast<int>(r), static_cast<int>(c) + 1, zero_on_solid);
                    field[i] = (source[i] + a * sum) / (1.0f + 4.0f * a);
                }
            }
            apply_solid_boundaries(field);
        }
    }

    auto bilinear_sample(const std::vector<float>& field, float y, float x) const -> float {
        int x0 = static_cast<int>(std::floor(x));
        int y0 = static_cast<int>(std::floor(y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        float sx = x - static_cast<float>(x0);
        float sy = y - static_cast<float>(y0);

        auto sample = [&](int r, int c) -> float {
            if (is_solid_cell(r, c)) {
                return 0.0f;
            }
            return field[idx(static_cast<uint32_t>(r), static_cast<uint32_t>(c))];
        };

        float q00 = sample(y0, x0);
        float q01 = sample(y0, x1);
        float q10 = sample(y1, x0);
        float q11 = sample(y1, x1);

        float top = q00 * (1.0f - sx) + q01 * sx;
        float bot = q10 * (1.0f - sx) + q11 * sx;
        return top * (1.0f - sy) + bot * sy;
    }

    void advect(std::vector<float>& field, const std::vector<float>& source,
                const std::vector<float>& vel_u, const std::vector<float>& vel_v,
                float dt) {
        std::vector<float> out(field.size(), 0.0f);
        float scale = dt * static_cast<float>(N - 2);

        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }

                float x = static_cast<float>(c) - scale * vel_u[i];
                float y = static_cast<float>(r) - scale * vel_v[i];
                x = std::clamp(x, 0.5f, static_cast<float>(N) - 1.5f);
                y = std::clamp(y, 0.5f, static_cast<float>(N) - 1.5f);
                out[i] = bilinear_sample(source, y, x);
            }
        }

        field.swap(out);
        apply_solid_boundaries(field);
    }

    void enforce_no_penetration() {
        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }
                if (is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) && u[i] > 0.0f) u[i] = 0.0f;
                if (is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) && u[i] < 0.0f) u[i] = 0.0f;
                if (is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c)) && v[i] > 0.0f) v[i] = 0.0f;
                if (is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c)) && v[i] < 0.0f) v[i] = 0.0f;
            }
        }
    }

    void project(uint32_t iters) {
        std::fill(pressure.begin(), pressure.end(), 0.0f);
        std::fill(divergence.begin(), divergence.end(), 0.0f);

        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }
                float u_l = is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) ? 0.0f : u[idx(r, c - 1)];
                float u_r = is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) ? 0.0f : u[idx(r, c + 1)];
                float v_u = is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c)) ? 0.0f : v[idx(r - 1, c)];
                float v_d = is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c)) ? 0.0f : v[idx(r + 1, c)];
                divergence[i] = -0.5f * ((u_r - u_l) + (v_d - v_u)) / static_cast<float>(N);
            }
        }

        for (uint32_t iter = 0; iter < iters; ++iter) {
            for (uint32_t r = 1; r + 1 < N; ++r) {
                for (uint32_t c = 1; c + 1 < N; ++c) {
                    uint32_t i = idx(r, c);
                    if (solid[i]) {
                        continue;
                    }
                    float sum = 0.0f;
                    float count = 0.0f;
                    if (!is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c))) {
                        sum += pressure[idx(r - 1, c)];
                        count += 1.0f;
                    }
                    if (!is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c))) {
                        sum += pressure[idx(r + 1, c)];
                        count += 1.0f;
                    }
                    if (!is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1)) {
                        sum += pressure[idx(r, c - 1)];
                        count += 1.0f;
                    }
                    if (!is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1)) {
                        sum += pressure[idx(r, c + 1)];
                        count += 1.0f;
                    }
                    if (count > 0.0f) {
                        pressure[i] = (divergence[i] + sum) / count;
                    }
                }
            }
        }

        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }
                float p_l = is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) ? pressure[i] : pressure[idx(r, c - 1)];
                float p_r = is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) ? pressure[i] : pressure[idx(r, c + 1)];
                float p_u = is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c)) ? pressure[i] : pressure[idx(r - 1, c)];
                float p_d = is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c)) ? pressure[i] : pressure[idx(r + 1, c)];
                u[i] -= 0.5f * static_cast<float>(N) * (p_r - p_l);
                v[i] -= 0.5f * static_cast<float>(N) * (p_d - p_u);
            }
        }

        apply_solid_boundaries(u);
        apply_solid_boundaries(v);
        enforce_no_penetration();
    }

    void settle_density() {
        std::vector<float> next = density;

        for (int r = static_cast<int>(N) - 2; r >= 1; --r) {
            for (int c = 1; c < static_cast<int>(N) - 1; ++c) {
                uint32_t i = idx(static_cast<uint32_t>(r), static_cast<uint32_t>(c));
                if (solid[i] || density[i] < 0.01f) {
                    continue;
                }

                auto try_move = [&](int nr, int nc, float fraction) {
                    if (is_solid_cell(nr, nc)) {
                        return;
                    }
                    uint32_t j = idx(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc));
                    float capacity = std::max(0.0f, 1.15f - next[j]);
                    if (capacity <= 0.0f) {
                        return;
                    }
                    float amount = std::min(next[i] * fraction, capacity);
                    next[i] -= amount;
                    next[j] += amount;
                };

                try_move(r + 1, c, 0.42f);
                if (next[i] > 0.02f) {
                    if (u[i] >= 0.0f) {
                        try_move(r + 1, c + 1, 0.18f);
                        try_move(r, c + 1, 0.07f);
                    } else {
                        try_move(r + 1, c - 1, 0.18f);
                        try_move(r, c - 1, 0.07f);
                    }
                }
            }
        }

        density.swap(next);
    }

    auto compute_stats() const -> FlowStats {
        FlowStats stats;
        uint32_t basin_cells = 0;
        uint32_t filled_cells = 0;
        uint32_t fluid_cells = 0;

        for (uint32_t r = 1; r + 1 < N; ++r) {
            for (uint32_t c = 1; c + 1 < N; ++c) {
                uint32_t i = idx(r, c);
                if (solid[i]) {
                    continue;
                }
                ++fluid_cells;
                float speed = std::sqrt(u[i] * u[i] + v[i] * v[i]);
                stats.max_speed = std::max(stats.max_speed, speed);
                stats.kinetic += 0.5f * density[i] * speed * speed;

                float u_l = is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) ? 0.0f : u[idx(r, c - 1)];
                float u_r = is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) ? 0.0f : u[idx(r, c + 1)];
                float v_u = is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c)) ? 0.0f : v[idx(r - 1, c)];
                float v_d = is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c)) ? 0.0f : v[idx(r + 1, c)];
                float local_div = 0.5f * ((u_r - u_l) + (v_d - v_u)) / static_cast<float>(N);
                stats.divergence_l1 += std::abs(local_div);

                float dv_dx = 0.5f * (v_r(c, r) - v_l(c, r));
                float du_dy = 0.5f * (u_d(c, r) - u_u(c, r));
                stats.mean_curl += std::abs(dv_dx - du_dy);

                if (basin[i]) {
                    ++basin_cells;
                    stats.mass_in_basin += density[i];
                    if (density[i] > 0.06f) {
                        ++filled_cells;
                    }
                }
            }
        }

        if (fluid_cells > 0) {
            stats.kinetic /= static_cast<float>(fluid_cells);
            stats.divergence_l1 /= static_cast<float>(fluid_cells);
            stats.mean_curl /= static_cast<float>(fluid_cells);
        }
        if (basin_cells > 0) {
            stats.fill_ratio = static_cast<float>(filled_cells) / static_cast<float>(basin_cells);
            stats.mass_in_basin /= static_cast<float>(basin_cells);
        }

        return stats;
    }

    auto u_l(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) ? 0.0f : u[idx(r, c - 1)];
    }

    auto u_r(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) ? 0.0f : u[idx(r, c + 1)];
    }

    auto u_u(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r) - 1, static_cast<int>(c)) ? 0.0f : u[idx(r - 1, c)];
    }

    auto u_d(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r) + 1, static_cast<int>(c)) ? 0.0f : u[idx(r + 1, c)];
    }

    auto v_l(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r), static_cast<int>(c) - 1) ? 0.0f : v[idx(r, c - 1)];
    }

    auto v_r(uint32_t c, uint32_t r) const -> float {
        return is_solid_cell(static_cast<int>(r), static_cast<int>(c) + 1) ? 0.0f : v[idx(r, c + 1)];
    }

    void step(const SimParams& params, uint32_t step_index) {
        add_inlet(params, step_index);
        apply_forces(params);

        u_prev = u;
        v_prev = v;
        diffuse(u, u_prev, params.viscosity, params.dt, params.pressure_iters, true);
        diffuse(v, v_prev, params.viscosity, params.dt, params.pressure_iters, true);
        project(params.pressure_iters);

        u_prev = u;
        v_prev = v;
        advect(u, u_prev, u_prev, v_prev, params.dt);
        advect(v, v_prev, u_prev, v_prev, params.dt);
        project(params.pressure_iters);

        density_prev = density;
        diffuse(density, density_prev, params.diffusion, params.dt, 8, false);
        density_prev = density;
        advect(density, density_prev, u, v, params.dt);
        settle_density();

        for (uint32_t i = 0; i < density.size(); ++i) {
            if (!solid[i]) {
                float speed = std::sqrt(u[i] * u[i] + v[i] * v[i]);
                density[i] *= basin[i] ? 0.99998f : params.density_decay;
                residence[i] = std::max(residence[i] * 0.985f,
                                        clamp01(0.75f * density[i] + 0.22f * speed));
            }
        }

        clamp_density();

        FlowStats stats = compute_stats();
        kinetic_history.push_back(stats.kinetic);
        fill_history.push_back(stats.fill_ratio);
        divergence_history.push_back(stats.divergence_l1);
        circulation_history.push_back(stats.mean_curl);
    }

    auto max_density() const -> float {
        float peak = 0.0f;
        for (uint32_t i = 0; i < density.size(); ++i) {
            if (!solid[i]) {
                peak = std::max(peak, density[i]);
            }
        }
        return peak;
    }
};

static auto flow_glyph(float u, float v) -> const char* {
    float ax = std::abs(u);
    float ay = std::abs(v);
    if (ax < 0.08f && ay < 0.08f) return "·";
    if (ax > ay * 1.35f) return (u >= 0.0f) ? "→" : "←";
    if (ay > ax * 1.35f) return (v >= 0.0f) ? "↓" : "↑";
    if (u >= 0.0f && v >= 0.0f) return "↘";
    if (u >= 0.0f && v < 0.0f) return "↗";
    if (u < 0.0f && v >= 0.0f) return "↙";
    return "↖";
}

static auto fluid_char(float density, float u, float v, float residence)
    -> std::pair<const char*, const char*> {
    float speed = std::sqrt(u * u + v * v);
    if (density > 0.76f) return {ansi::water_hi, "█"};
    if (density > 0.46f) return {ansi::water_hi, speed > 0.38f ? flow_glyph(u, v) : "▓"};
    if (density > 0.22f) return {ansi::water_mid, speed > 0.32f ? flow_glyph(u, v) : "▒"};
    if (density > 0.08f) return {ansi::water_lo, speed > 0.24f ? flow_glyph(u, v) : "░"};
    if (speed > 0.30f) return {ansi::spray, flow_glyph(u, v)};
    if (residence > 0.14f) return {ansi::mist, "·"};
    return {ansi::calm, " "};
}

static void render_bar(const char* label, float value, float max_value,
                       int width, const char* color) {
    int filled = 0;
    if (max_value > 1e-8f) {
        filled = static_cast<int>(value / max_value * static_cast<float>(width));
    }
    filled = std::clamp(filled, 0, width);
    std::printf("  %s:%s ", label, ansi::reset);
    std::printf("%s", color);
    for (int i = 0; i < filled; ++i) std::printf(" ");
    std::printf("%s", ansi::reset);
    for (int i = filled; i < width; ++i) std::printf("░");
}

static void render_sparkline(const char* label, const std::vector<float>& hist,
                             int width, const char* hi_color, const char* lo_color) {
    if (hist.empty()) return;
    static const char* bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int start = static_cast<int>(hist.size()) > width
        ? static_cast<int>(hist.size()) - width : 0;
    float mx = *std::max_element(hist.begin() + start, hist.end());
    if (mx < 1e-8f) {
        mx = 1.0f;
    }

    std::printf("  %s:%s ", label, ansi::reset);
    for (int i = start; i < static_cast<int>(hist.size()); ++i) {
        float norm = hist[i] / mx;
        int level = std::clamp(static_cast<int>(norm * 7.99f), 0, 7);
        std::printf("%s%s", norm > 0.55f ? hi_color : lo_color, bars[level]);
    }
    std::printf("%s\n", ansi::reset);
}

static void render_domain(const FlowGrid& grid, bool inlet_active) {
    std::printf("  %s┌", ansi::border);
    for (uint32_t c = 0; c < grid.N; ++c) std::printf("──");
    std::printf("┐%s\n", ansi::reset);

    for (uint32_t r = 0; r < grid.N; ++r) {
        std::printf("  %s│%s", ansi::border, ansi::reset);
        for (uint32_t c = 0; c < grid.N; ++c) {
            uint32_t i = grid.idx(r, c);
            if (grid.nozzle[i]) {
                std::printf("%s%s ", ansi::nozzle, inlet_active ? "▶" : "•");
            } else if (grid.solid[i]) {
                std::printf("%s█ ", ansi::wall);
            } else {
                auto [color, ch] = fluid_char(grid.density[i], grid.u[i], grid.v[i], grid.residence[i]);
                std::printf("%s%s ", color, ch);
            }
        }
        std::printf("%s│%s\n", ansi::border, ansi::reset);
    }

    std::printf("  %s└", ansi::border);
    for (uint32_t c = 0; c < grid.N; ++c) std::printf("──");
    std::printf("┘%s\n", ansi::reset);
}

struct FluidAgent {
    uint32_t id{0};
    void on_receive(Envelope, ActorContext&) {}
};

static void update_task_graph(PheromoneGraph* graph, const FlowStats& stats,
                              uint32_t step, uint32_t inflow_steps) {
    if (graph == nullptr) {
        return;
    }

    graph->mark_status(TaskNodeId{1}, step < inflow_steps ? TaskStatus::InProgress : TaskStatus::Completed);

    if (stats.fill_ratio > 0.03f) {
        graph->mark_status(TaskNodeId{2}, TaskStatus::InProgress);
    }
    if (stats.fill_ratio > 0.28f) {
        graph->mark_status(TaskNodeId{2}, TaskStatus::Completed);
    }

    if (step > 8) {
        graph->mark_status(TaskNodeId{3}, TaskStatus::InProgress);
    }
    if (stats.divergence_l1 < 0.012f && step > 30) {
        graph->mark_status(TaskNodeId{3}, TaskStatus::Completed);
    }

    if (stats.fill_ratio > 0.52f) {
        graph->mark_status(TaskNodeId{4}, TaskStatus::InProgress);
    }
    if (stats.fill_ratio > 0.80f && stats.max_speed < 0.20f) {
        graph->mark_status(TaskNodeId{4}, TaskStatus::Completed);
    }

    if (step < inflow_steps) {
        graph->deposit({TaskNodeId{0}, TaskNodeId{1}}, 0.40f + 0.60f * stats.max_speed);
    }
    if (stats.fill_ratio > 0.02f) {
        graph->deposit({TaskNodeId{1}, TaskNodeId{2}}, 0.20f + stats.fill_ratio);
    }
    if (stats.divergence_l1 < 0.10f) {
        graph->deposit({TaskNodeId{2}, TaskNodeId{3}}, 1.0f - std::min(0.95f, stats.divergence_l1 * 8.0f));
    }
    if (stats.fill_ratio > 0.35f) {
        graph->deposit({TaskNodeId{3}, TaskNodeId{4}}, 0.25f + stats.mass_in_basin);
    }
}

int main(int argc, char* argv[]) {
    uint32_t grid_n = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 34;
    float jet_scale = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 1.0f;

    grid_n = std::clamp(grid_n, 18u, 52u);
    jet_scale = std::clamp(jet_scale, 0.60f, 1.60f);

    constexpr uint32_t total_steps = 360;
    constexpr int frame_ms = 45;
    constexpr uint32_t strat_dim = 3;

    SimParams params;
    params.inlet_u *= jet_scale;
    params.inlet_v *= jet_scale;
    params.inlet_density = std::min(0.80f, params.inlet_density * (0.92f + 0.20f * jet_scale));

    uint32_t population = grid_n * grid_n;

    SwarmConfig cfg;
    cfg.population = population;
    cfg.strategy_dim = strat_dim;
    cfg.alignment_radius = 1.5f;
    cfg.noise_eta = 0.12f;
    cfg.evaporation_rate = 0.015f;
    cfg.convergence_phi = 0.985f;
    cfg.sustain_window = 14;
    cfg.max_rounds = total_steps;
    cfg.premature_quality = 0.12f;
    cfg.noise_burst = 0.18f;
    cfg.morphs = {
        {"Explorer", 0.58f, 0.14f},
        {"Worker", 0.42f, 0.10f},
        {"Evaluator", 0.28f, 0.09f},
        {"Coordinator", 0.48f, 0.11f},
    };
    cfg.channel_capacity = 256;

    ActorSystem sys(1024);
    auto ts = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    Swarm swarm(cfg, sys);
    swarm.field() = SwarmField(population, strat_dim, cfg.alignment_radius, cfg.noise_eta, ts);

    auto proto = sys.spawn<FluidAgent>("proto", FluidAgent{0});
    sys.spawn_n(proto, population, "fluid");

    TaskNode root;
    root.id = TaskNodeId{0};
    root.description = "navier-stokes-container";
    root.status = TaskStatus::InProgress;

    auto& cluster = swarm.cluster_service().create_cluster(
        "container_flow", root, cfg.evaporation_rate, 1.0f, 1.0f, 2.0f, ts + 1);
    auto* graph = swarm.cluster_service().graph_for(cluster);

    if (graph != nullptr) {
        std::vector<std::pair<uint32_t, std::string>> nodes = {
            {1, "inlet forcing"},
            {2, "advection + diffusion"},
            {3, "pressure projection"},
            {4, "basin settling"},
        };
        for (const auto& [id, description] : nodes) {
            TaskNode node;
            node.id = TaskNodeId{id};
            node.description = description;
            node.parent = TaskNodeId{0};
            node.depth = 1;
            graph->add_node(std::move(node));
        }
        graph->ensure_edge(TaskNodeId{0}, TaskNodeId{1});
        graph->ensure_edge(TaskNodeId{1}, TaskNodeId{2});
        graph->ensure_edge(TaskNodeId{2}, TaskNodeId{3});
        graph->ensure_edge(TaskNodeId{3}, TaskNodeId{4});
    }

    swarm.cluster_service().recruit(sys, swarm.field(), cluster, swarm.field().agents(), population);

    {
        std::mt19937 trng(static_cast<uint32_t>(ts + 2));
        std::normal_distribution<float> jitter(0.0f, 0.10f);
        auto& thresholds = swarm.scheduler().thresholds();
        for (uint32_t i = 0; i < population && i < thresholds.size(); ++i) {
            for (uint8_t c = 0; c < morph_count; ++c) {
                thresholds[i][c] = std::max(0.05f, thresholds[i][c] + jitter(trng));
            }
        }
    }

    FlowGrid grid(grid_n);

    std::printf("%s", ansi::clear);
    std::printf("\n  %s═══ celer::necto::swarm ─── Navier-Stokes Container Flow "
                "═══════════════════%s\n\n", ansi::border, ansi::reset);
    std::printf("  %s2D Incompressible Flow%s:  ∂u/∂t + (u·∇)u = -∇p + ν∇²u + f,  ∇·u = 0\n\n",
        ansi::header, ansi::reset);
    std::printf("  Grid:     %s%u×%u%s (%u agents)\n",
        ansi::stat_val, grid_n, grid_n, ansi::reset, population);
    std::printf("  Jet:      %s%.2f%s   ν: %s%.5f%s   Δt: %s%.2f%s   g: %s%.2f%s\n",
        ansi::stat_val, jet_scale, ansi::reset,
        ansi::stat_val, params.viscosity, ansi::reset,
        ansi::stat_val, params.dt, ansi::reset,
        ansi::stat_val, params.gravity, ansi::reset);
    std::printf("  Strategy: R^%u (vx, vy, fill)\n\n", strat_dim);
    std::printf("  %sVicsek alignment = coherent advection   |   η noise = unresolved eddies%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sPheromone = wetted-path memory   |   φ → 1.0 = quiescent basin fill%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sExplorer=jet  Worker=walls  Evaluator=projection  Coordinator=settling%s\n\n",
        ansi::dim, ansi::reset);
    std::printf("  %s════════════════════════════════════════════════════════════════════════════%s\n",
        ansi::border, ansi::reset);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    float peak_kinetic = 0.0f;
    float peak_fill = 0.0f;
    float worst_divergence = 0.0f;
    uint32_t last_step = 0;

    for (uint32_t step = 0; step < total_steps; ++step) {
        last_step = step;
        grid.step(params, step);
        FlowStats stats = grid.compute_stats();

        peak_kinetic = std::max(peak_kinetic, stats.kinetic);
        peak_fill = std::max(peak_fill, stats.fill_ratio);
        worst_divergence = std::max(worst_divergence, stats.divergence_l1);

        update_task_graph(graph, stats, step, params.inflow_steps);

        float max_speed = std::max(0.01f, stats.max_speed);
        float max_density = std::max(0.01f, grid.max_density());
        for (uint32_t i = 0; i < population; ++i) {
            float nx = grid.u[i] / max_speed;
            float ny = grid.v[i] / max_speed;
            float nz = grid.density[i] / max_density;
            float norm = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (norm > 1e-6f) {
                swarm.field()[i].heading = {nx / norm, ny / norm, nz / norm};
            } else {
                swarm.field()[i].heading = {0.0f, 0.0f, 1.0f};
            }
            swarm.field()[i].quality = std::clamp(0.55f * nz + 0.45f * (std::sqrt(nx * nx + ny * ny)), 0.0f, 1.0f);
        }

        float eta = std::clamp(0.05f + 0.35f * stats.max_speed + 3.0f * stats.divergence_l1,
                               0.05f, 0.92f);
        swarm.field().set_eta(eta);
        swarm.tick_cluster(cluster);

        for (uint32_t i = 0; i < population; ++i) {
            swarm.field()[i].morph = swarm.scheduler().morph_of(i);
        }

        std::printf("%s", ansi::clear);
        std::printf("\n  %s═══ celer::necto::swarm ─── Navier-Stokes Container Flow "
                    "═══════════════════%s\n\n", ansi::border, ansi::reset);

        render_domain(grid, step < params.inflow_steps);
        std::printf("\n");

        render_bar("Fill", stats.fill_ratio, 1.0f, 30, ansi::fill_bar);
        std::printf("  %s%.1f%%%s basin occupancy\n",
            ansi::stat_val, stats.fill_ratio * 100.0f, ansi::reset);

        render_bar("Kinetic", stats.kinetic, std::max(peak_kinetic, 0.001f), 30, ansi::energy_bar);
        std::printf("  %s%.4f%s\n", ansi::stat_val, stats.kinetic, ansi::reset);

        render_bar("Divergence", stats.divergence_l1, std::max(worst_divergence, 0.001f), 30, ansi::div_bar);
        std::printf("  %s%.5f%s (projection residual)\n\n",
            ansi::stat_val, stats.divergence_l1, ansi::reset);

        render_sparkline("K(t)", grid.kinetic_history, 52, ansi::water_mid, ansi::water_lo);
        render_sparkline("Fill(t)", grid.fill_history, 52, ansi::ok, ansi::water_lo);
        render_sparkline("Curl(t)", grid.circulation_history, 52, ansi::warn, ansi::mist);
        std::printf("\n");

        std::printf("  Step %s%u%s/%u   η %s%.2f%s   φ %s%.3f%s   max|u| %s%.3f%s   mass %s%.3f%s\n",
            ansi::stat_val, step, ansi::reset, total_steps,
            ansi::stat_val, eta, ansi::reset,
            ansi::stat_val, cluster.phi, ansi::reset,
            ansi::stat_val, stats.max_speed, ansi::reset,
            ansi::stat_val, stats.mass_in_basin, ansi::reset);

        auto morph_dist = swarm.scheduler().distribution();
        std::printf("  %s●%s Jet %s%u%s  %s●%s Wall %s%u%s  %s●%s Proj %s%u%s  %s●%s Settle %s%u%s\n",
            ansi::explorer, ansi::reset, ansi::stat_val, morph_dist[0], ansi::reset,
            ansi::worker, ansi::reset, ansi::stat_val, morph_dist[1], ansi::reset,
            ansi::evaluator, ansi::reset, ansi::stat_val, morph_dist[2], ansi::reset,
            ansi::coord, ansi::reset, ansi::stat_val, morph_dist[3], ansi::reset);

        uint32_t complete = graph != nullptr ? graph->count_by_status(TaskStatus::Completed) : 0;
        uint32_t in_progress = graph != nullptr ? graph->count_by_status(TaskStatus::InProgress) : 0;
        std::printf("  Tasks %s%u%s done / %s%u%s active   Trails %s%u%s   Inlet %s%s%s\n",
            ansi::stat_val, complete, ansi::reset,
            ansi::stat_val, in_progress, ansi::reset,
            ansi::stat_val, graph != nullptr ? graph->edge_count() : 0, ansi::reset,
            step < params.inflow_steps ? ansi::warn : ansi::ok,
            step < params.inflow_steps ? "ON" : "OFF",
            ansi::reset);

        std::printf("\n  %s∂u/∂t + (u·∇)u = -∇p + ν∇²u + f,   ∇·u = 0%s\n",
            ansi::dim, ansi::reset);
        std::printf("  %sStable advection + diffusion + projection on a Cartesian basin%s\n",
            ansi::dim, ansi::reset);
        std::printf("  %sVicsek 1995 × ACO × Bonabeau 1996   —   celer-mem / necto%s\n",
            ansi::dim, ansi::reset);

        std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));

        if (step > params.inflow_steps + 12 && peak_fill > 0.15f
            && stats.fill_ratio < peak_fill * 0.58f && stats.max_speed < 0.24f) {
            break;
        }

        if (step > params.inflow_steps + 50 && stats.fill_ratio > 0.82f && stats.max_speed < 0.12f) {
            break;
        }
    }

    FlowStats final_stats = grid.compute_stats();
    std::printf("%s", ansi::clear);
    std::printf("\n  %s═══ Simulation Complete ══════════════════════════════════════════════════%s\n\n",
        ansi::border, ansi::reset);
    std::printf("  %s── Navier-Stokes Container Summary ──%s\n\n", ansi::header, ansi::reset);
    std::printf("  Grid:             %u×%u (%u agents)\n", grid_n, grid_n, population);
    std::printf("  Steps:            %u\n", last_step + 1);
    std::printf("  Jet scale:        %.2f\n", jet_scale);
    std::printf("  Viscosity ν:      %.5f\n", params.viscosity);
    std::printf("  Peak fill:        %.1f%%\n", peak_fill * 100.0f);
    std::printf("  Final fill:       %.1f%%\n", final_stats.fill_ratio * 100.0f);
    std::printf("  Peak kinetic:     %.4f\n", peak_kinetic);
    std::printf("  Final kinetic:    %.4f\n", final_stats.kinetic);
    std::printf("  Worst div L1:     %.5f\n", worst_divergence);
    std::printf("  Final div L1:     %.5f\n", final_stats.divergence_l1);
    std::printf("  Final mass basin: %.4f\n\n", final_stats.mass_in_basin);

    render_sparkline("K(t)", grid.kinetic_history, 60, ansi::water_mid, ansi::water_lo);
    render_sparkline("Fill(t)", grid.fill_history, 60, ansi::ok, ansi::water_lo);

    std::printf("\n  %sContainer:%s open-top basin with left-side jet and pressure projection\n",
        ansi::dim, ansi::reset);
    std::printf("  %sPrimitive mapping:%s jet/advection, wall shear, divergence audit, settling\n",
        ansi::dim, ansi::reset);
    std::printf("  %sNavier-Stokes expressed through swarm headings (vx, vy, fill)%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sVicsek 1995 × ACO × Bonabeau 1996   —   celer-mem / necto%s\n\n",
        ansi::dim, ansi::reset);

    return 0;
}

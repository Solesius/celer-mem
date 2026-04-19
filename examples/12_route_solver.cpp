/// celer::necto::swarm — TSP / Route Solver
///
/// A swarm of agents solves the Travelling Salesman Problem using the
/// engine's built-in ACO pheromone graph + Vicsek alignment + Bonabeau
/// morph scheduling.  Each agent constructs complete tours, deposits
/// pheromone on good edges, and aligns headings in R^N (tour-rank
/// encoding) so the swarm converges on the optimal route.
///
/// Physics mapping:
///   Vicsek alignment  = tour similarity consensus
///   η noise           = exploration vs exploitation
///   φ convergence     = all agents agree on same route
///   Pheromone trails  = edge quality (classic ACO τ)
///   Morphs:
///     Explorer    = random tour construction (high diversity)
///     Worker      = pheromone-weighted nearest-neighbor
///     Evaluator   = 2-opt local improvement
///     Coordinator = elite tour preservation
///
/// The task graph nodes are cities; edges carry pheromone (τ).
/// Tour construction: P(next=j) ∝ τ(i,j)^α · η(i,j)^β
/// where η(i,j) = 1/d(i,j) is the visibility heuristic.
///
/// Usage:  ./example_12_route_solver [n_cities] [seed]
/// Default: 20 cities, time-based seed

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "celer/necto/swarm/swarm.hpp"

using namespace celer::necto;
using namespace celer::necto::swarm;

// ── ANSI ──────────────────────────────────────────────────────

namespace ansi {
    constexpr const char* clear     = "\033[2J\033[H";
    constexpr const char* home      = "\033[H";
    constexpr const char* reset     = "\033[0m";
    constexpr const char* bold      = "\033[1m";
    constexpr const char* dim       = "\033[2m";
    constexpr const char* explorer  = "\033[36m";
    constexpr const char* worker    = "\033[33m";
    constexpr const char* evaluator = "\033[32m";
    constexpr const char* coord     = "\033[35m";
    constexpr const char* header    = "\033[1;37m";
    constexpr const char* stat_val  = "\033[1;96m";
    constexpr const char* ok        = "\033[1;32m";
    constexpr const char* warn      = "\033[1;33m";
    constexpr const char* danger    = "\033[1;31m";
    constexpr const char* border    = "\033[90m";
    constexpr const char* city_clr  = "\033[1;97m";
    constexpr const char* route_clr = "\033[1;36m";
    constexpr const char* best_clr  = "\033[1;32m";
    constexpr const char* phero_hi  = "\033[1;33m";
    constexpr const char* phero_md  = "\033[33m";
    constexpr const char* phero_lo  = "\033[2;33m";
    constexpr const char* improve   = "\033[1;32m";
    constexpr const char* stagnate  = "\033[33m";
    constexpr const char* depot_clr = "\033[1;31m";
}

// ── City ──────────────────────────────────────────────────────

struct City {
    float x, y;
    char label;
};

static float city_dist(const City& a, const City& b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// ── Tour ──────────────────────────────────────────────────────

static float tour_length(const std::vector<uint32_t>& tour,
                         const std::vector<City>& cities) {
    float len = 0;
    for (uint32_t i = 0; i < tour.size(); ++i) {
        uint32_t j = (i + 1) % tour.size();
        len += city_dist(cities[tour[i]], cities[tour[j]]);
    }
    return len;
}

// ── 2-opt local search ───────────────────────────────────────

static bool two_opt_pass(std::vector<uint32_t>& tour,
                         const std::vector<City>& cities) {
    uint32_t n = tour.size();
    bool improved = false;
    for (uint32_t i = 0; i < n - 1; ++i) {
        for (uint32_t j = i + 2; j < n; ++j) {
            if (j == n - 1 && i == 0) continue;  // skip full reversal
            float d_old = city_dist(cities[tour[i]], cities[tour[i+1]])
                        + city_dist(cities[tour[j]], cities[tour[(j+1)%n]]);
            float d_new = city_dist(cities[tour[i]], cities[tour[j]])
                        + city_dist(cities[tour[i+1]], cities[tour[(j+1)%n]]);
            if (d_new < d_old - 1e-6f) {
                std::reverse(tour.begin() + i + 1, tour.begin() + j + 1);
                improved = true;
            }
        }
    }
    return improved;
}

// ── Pheromone-weighted construction ───────────────────────────
// P(next=j) ∝ τ(i,j)^α · (1/d(i,j))^β

static auto construct_tour(uint32_t n_cities,
                           const std::vector<City>& cities,
                           const std::vector<std::vector<float>>& tau,
                           float alpha, float beta,
                           std::mt19937& rng,
                           uint32_t start = 0) -> std::vector<uint32_t> {
    std::vector<uint32_t> tour;
    tour.reserve(n_cities);
    std::vector<bool> visited(n_cities, false);

    tour.push_back(start);
    visited[start] = true;

    for (uint32_t step = 1; step < n_cities; ++step) {
        uint32_t cur = tour.back();
        std::vector<float> probs(n_cities, 0.0f);
        float total = 0.0f;

        for (uint32_t j = 0; j < n_cities; ++j) {
            if (visited[j]) continue;
            float d = city_dist(cities[cur], cities[j]);
            float vis = (d > 1e-6f) ? 1.0f / d : 1e6f;
            float p = std::pow(tau[cur][j], alpha) * std::pow(vis, beta);
            probs[j] = p;
            total += p;
        }

        if (total < 1e-12f) {
            // Fallback: pick first unvisited
            for (uint32_t j = 0; j < n_cities; ++j) {
                if (!visited[j]) { tour.push_back(j); visited[j] = true; break; }
            }
            continue;
        }

        std::uniform_real_distribution<float> u(0.0f, total);
        float r = u(rng);
        float cumul = 0.0f;
        for (uint32_t j = 0; j < n_cities; ++j) {
            if (visited[j]) continue;
            cumul += probs[j];
            if (cumul >= r) {
                tour.push_back(j);
                visited[j] = true;
                break;
            }
        }
        if (tour.size() <= step) {
            // Edge case: pick last unvisited
            for (uint32_t j = 0; j < n_cities; ++j)
                if (!visited[j]) { tour.push_back(j); visited[j] = true; break; }
        }
    }
    return tour;
}

// ── Tour to heading (R^N rank encoding) ───────────────────────

static auto tour_to_heading(const std::vector<uint32_t>& tour,
                            uint32_t n_cities) -> std::vector<float> {
    std::vector<float> h(n_cities, 0.0f);
    for (uint32_t i = 0; i < tour.size(); ++i)
        h[tour[i]] = static_cast<float>(i) / static_cast<float>(n_cities);
    // Normalize to unit vector
    float nm = 0;
    for (auto x : h) nm += x * x;
    nm = std::sqrt(nm);
    if (nm > 1e-6f) for (auto& x : h) x /= nm;
    return h;
}

// ── Render 2D map ─────────────────────────────────────────────

static void render_map(const std::vector<City>& cities,
                       const std::vector<uint32_t>& best_tour,
                       const std::vector<std::vector<float>>& tau,
                       uint32_t map_w, uint32_t map_h) {
    // Canvas
    struct Cell { const char* ch = " "; const char* fg = ""; int prio = 0; };
    std::vector<Cell> canvas(map_w * map_h);

    auto plot = [&](int x, int y, const char* ch, const char* fg, int prio) {
        if (x < 0 || x >= (int)map_w || y < 0 || y >= (int)map_h) return;
        auto& c = canvas[y * map_w + x];
        if (prio >= c.prio) c = {ch, fg, prio};
    };

    // Find pheromone range for heat coloring
    float tau_max = 0;
    uint32_t nc = cities.size();
    for (uint32_t i = 0; i < nc; ++i)
        for (uint32_t j = i + 1; j < nc; ++j)
            tau_max = std::max(tau_max, tau[i][j]);
    if (tau_max < 1e-8f) tau_max = 1.0f;

    // Draw pheromone-heavy edges (top 15% by pheromone)
    float tau_thresh = tau_max * 0.35f;
    for (uint32_t i = 0; i < nc; ++i) {
        for (uint32_t j = i + 1; j < nc; ++j) {
            if (tau[i][j] < tau_thresh) continue;
            float norm_tau = tau[i][j] / tau_max;
            const char* ec = norm_tau > 0.7f ? ansi::phero_hi
                           : norm_tau > 0.4f ? ansi::phero_md
                           : ansi::phero_lo;
            // Bresenham
            int x0 = (int)(cities[i].x / 100.0f * (float)(map_w - 1));
            int y0 = (int)(cities[i].y / 100.0f * (float)(map_h - 1));
            int x1 = (int)(cities[j].x / 100.0f * (float)(map_w - 1));
            int y1 = (int)(cities[j].y / 100.0f * (float)(map_h - 1));
            int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            int cx = x0, cy = y0;
            for (int s = 0; s < 300; ++s) {
                plot(cx, cy, "·", ec, 1);
                if (cx == x1 && cy == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; cx += sx; }
                if (e2 <= dx) { err += dx; cy += sy; }
            }
        }
    }

    // Draw best tour route
    if (!best_tour.empty()) {
        for (uint32_t i = 0; i < best_tour.size(); ++i) {
            uint32_t a = best_tour[i];
            uint32_t b = best_tour[(i + 1) % best_tour.size()];
            int x0 = (int)(cities[a].x / 100.0f * (float)(map_w - 1));
            int y0 = (int)(cities[a].y / 100.0f * (float)(map_h - 1));
            int x1 = (int)(cities[b].x / 100.0f * (float)(map_w - 1));
            int y1 = (int)(cities[b].y / 100.0f * (float)(map_h - 1));
            int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            int cx = x0, cy = y0;
            for (int s = 0; s < 300; ++s) {
                plot(cx, cy, "─", ansi::best_clr, 5);
                if (cx == x1 && cy == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; cx += sx; }
                if (e2 <= dx) { err += dx; cy += sy; }
            }
        }
    }

    // Draw cities on top
    for (uint32_t i = 0; i < nc; ++i) {
        int x = (int)(cities[i].x / 100.0f * (float)(map_w - 1));
        int y = (int)(cities[i].y / 100.0f * (float)(map_h - 1));
        char buf[4]; buf[0] = cities[i].label; buf[1] = 0;
        // Use static storage for label strings
        static char labels[64][2];
        labels[i][0] = cities[i].label;
        labels[i][1] = 0;
        const char* clr = (i == 0) ? ansi::depot_clr : ansi::city_clr;
        plot(x, y, labels[i], clr, 10);
    }

    // Render
    std::printf("  %s┌", ansi::border);
    for (uint32_t c = 0; c < map_w; ++c) std::printf("─");
    std::printf("┐%s\n", ansi::reset);

    for (uint32_t r = 0; r < map_h; ++r) {
        std::printf("  %s│%s", ansi::border, ansi::reset);
        const char* prev = "";
        for (uint32_t c = 0; c < map_w; ++c) {
            auto& cl = canvas[r * map_w + c];
            if (cl.fg != prev) { std::printf("%s", cl.fg); prev = cl.fg; }
            std::printf("%s", cl.ch);
        }
        std::printf("%s│%s\n", ansi::border, ansi::reset);
    }

    std::printf("  %s└", ansi::border);
    for (uint32_t c = 0; c < map_w; ++c) std::printf("─");
    std::printf("┘%s\n", ansi::reset);
}

// ── Sparkline ─────────────────────────────────────────────────

static void render_sparkline(const char* label,
                             const std::vector<float>& hist,
                             int width, const char* color,
                             bool invert = false) {
    if (hist.empty()) return;
    static const char* bars[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    int s = (int)hist.size() > width ? (int)hist.size() - width : 0;
    float mx = *std::max_element(hist.begin() + s, hist.end());
    float mn = *std::min_element(hist.begin() + s, hist.end());
    float rng = mx - mn;
    if (rng < 1e-8f) rng = 1.0f;
    std::printf("  %s%-8s%s ", ansi::dim, label, ansi::reset);
    for (int i = s; i < (int)hist.size(); ++i) {
        float norm = (hist[i] - mn) / rng;
        if (invert) norm = 1.0f - norm;
        int lv = std::clamp((int)(norm * 7.99f), 0, 7);
        std::printf("%s%s", color, bars[lv]);
    }
    std::printf("%s\n", ansi::reset);
}

// ── Trivial actor ─────────────────────────────────────────────

struct TruckAgent {
    uint32_t id{0};
    void on_receive(Envelope, ActorContext&) {}
};

// ══════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    uint32_t N_CITIES = (argc > 1) ? (uint32_t)std::atoi(argv[1]) : 20;
    uint32_t SEED     = (argc > 2) ? (uint32_t)std::atoi(argv[2]) : 0;

    N_CITIES = std::clamp(N_CITIES, 6u, 52u);

    auto ts = (uint64_t)std::chrono::steady_clock::now()
                  .time_since_epoch().count();
    if (SEED == 0) SEED = (uint32_t)(ts & 0xFFFFFFFF);

    // ── Tunables ──────────────────────────────────────────────
    constexpr uint32_t POP         = 64;
    constexpr float    ACO_ALPHA   = 1.5f;     // pheromone weight
    constexpr float    ACO_BETA    = 3.0f;     // distance heuristic weight
    constexpr float    EVAP_RATE   = 0.12f;    // pheromone evaporation
    constexpr float    Q_DEPOSIT   = 100.0f;   // pheromone deposit constant
    constexpr float    TAU_INIT    = 0.1f;     // initial pheromone
    constexpr float    TAU_MIN     = 0.01f;    // min pheromone (MMAS)
    constexpr float    TAU_MAX     = 10.0f;    // max pheromone (MMAS)
    constexpr uint32_t MAX_ROUNDS  = 200;
    constexpr int      FRAME_MS    = 80;
    constexpr float    ETA_HI      = 2.0f;     // initial noise (exploration)
    constexpr float    ETA_LO      = 0.05f;    // final noise (exploitation)
    constexpr uint32_t MAP_W       = 70;
    constexpr uint32_t MAP_H       = 25;

    // ── Generate cities ───────────────────────────────────────
    std::mt19937 rng(SEED);
    std::vector<City> cities(N_CITIES);
    {
        std::uniform_real_distribution<float> pos(5.0f, 95.0f);
        for (uint32_t i = 0; i < N_CITIES; ++i) {
            cities[i].x = pos(rng);
            cities[i].y = pos(rng);
            // Label: A-Z then a-z
            cities[i].label = i < 26
                ? static_cast<char>('A' + i)
                : static_cast<char>('a' + i - 26);
        }
    }

    // Pre-compute distance matrix
    std::vector<std::vector<float>> dist(N_CITIES,
        std::vector<float>(N_CITIES, 0.0f));
    for (uint32_t i = 0; i < N_CITIES; ++i)
        for (uint32_t j = 0; j < N_CITIES; ++j)
            dist[i][j] = city_dist(cities[i], cities[j]);

    // Pheromone matrix (separate from engine pheromone — we need float[][])
    std::vector<std::vector<float>> tau(N_CITIES,
        std::vector<float>(N_CITIES, TAU_INIT));

    // Nearest-neighbor heuristic for initial upper bound
    std::vector<uint32_t> nn_tour(N_CITIES);
    std::iota(nn_tour.begin(), nn_tour.end(), 0u);
    {
        std::vector<bool> vis(N_CITIES, false);
        std::vector<uint32_t> t;
        t.push_back(0); vis[0] = true;
        for (uint32_t step = 1; step < N_CITIES; ++step) {
            uint32_t cur = t.back();
            float best_d = 1e18f;
            uint32_t best_j = 0;
            for (uint32_t j = 0; j < N_CITIES; ++j) {
                if (vis[j]) continue;
                if (dist[cur][j] < best_d) { best_d = dist[cur][j]; best_j = j; }
            }
            t.push_back(best_j);
            vis[best_j] = true;
        }
        nn_tour = t;
    }
    float nn_length = tour_length(nn_tour, cities);
    // 2-opt improve the NN tour
    for (int pass = 0; pass < 5; ++pass) two_opt_pass(nn_tour, cities);
    float nn_opt_length = tour_length(nn_tour, cities);

    // ── Swarm config ──────────────────────────────────────────
    SwarmConfig cfg;
    cfg.population       = POP;
    cfg.strategy_dim     = N_CITIES;  // R^N heading
    cfg.alignment_radius = 1.0f;
    cfg.noise_eta        = ETA_HI;
    cfg.evaporation_rate = EVAP_RATE;
    cfg.convergence_phi  = 0.95f;
    cfg.sustain_window   = 10;
    cfg.max_rounds       = MAX_ROUNDS;
    cfg.premature_quality = 0.3f;
    cfg.noise_burst      = 0.3f;
    cfg.morphs           = {
        {"Explorer",    0.8f, 0.15f},   // random construction
        {"Worker",      0.3f, 0.10f},   // pheromone-greedy
        {"Evaluator",   0.4f, 0.10f},   // 2-opt improvement
        {"Coordinator", 0.7f, 0.12f},   // elite preservation
    };
    cfg.channel_capacity = 256;

    ActorSystem sys(256);
    Swarm swarm(cfg, sys);
    swarm.field() = SwarmField(POP, N_CITIES, cfg.alignment_radius,
                               cfg.noise_eta, ts);

    auto proto = sys.spawn<TruckAgent>("proto", TruckAgent{0});
    sys.spawn_n(proto, POP, "truck");

    // ── Cluster + task graph (cities as nodes) ────────────────
    TaskNode root;
    root.id = TaskNodeId{0};
    root.description = "route-optimize";
    root.status = TaskStatus::Unexplored;

    auto& cl = swarm.cluster_service().create_cluster(
        "TSP-solver", root, EVAP_RATE, 1.0f, 1.0f, 2.0f, ts + 1);
    auto* gr = swarm.cluster_service().graph_for(cl);

    for (uint32_t i = 0; i < N_CITIES; ++i) {
        TaskNode nd;
        nd.id          = TaskNodeId{i + 1};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "city-%c", cities[i].label);
        nd.description = buf;
        nd.parent      = TaskNodeId{0};
        nd.depth       = 1;
        nd.status      = TaskStatus::Unexplored;
        gr->add_node(std::move(nd));
        gr->ensure_edge(TaskNodeId{0}, TaskNodeId{i + 1});
    }
    // Edges between adjacent cities in tour order
    for (uint32_t i = 0; i < N_CITIES; ++i)
        for (uint32_t j = i + 1; j < N_CITIES; ++j)
            gr->ensure_edge(TaskNodeId{i + 1}, TaskNodeId{j + 1});

    swarm.cluster_service().recruit(
        sys, swarm.field(), cl, swarm.field().agents(), POP);

    {
        std::mt19937 trng((uint32_t)(ts + 2));
        std::normal_distribution<float> nd(0.0f, 0.10f);
        auto& th = swarm.scheduler().thresholds();
        for (uint32_t i = 0; i < POP && i < th.size(); ++i)
            for (uint8_t c = 0; c < morph_count; ++c)
                th[i][c] = std::max(0.05f, th[i][c] + nd(trng));
    }

    // ── Agent tour storage ────────────────────────────────────
    std::vector<std::vector<uint32_t>> agent_tours(POP);
    std::vector<float> agent_lengths(POP, 1e18f);

    // Global best
    std::vector<uint32_t> best_tour = nn_tour;
    float best_length = nn_opt_length;

    // History
    std::vector<float> best_hist, avg_hist, phi_hist, div_hist;

    // ── Intro ─────────────────────────────────────────────────
    std::printf("%s", ansi::clear);
    std::printf("\n  %s═══ celer::necto::swarm ─── TSP Route Solver "
                "══════════════════════════════%s\n\n",
                ansi::border, ansi::reset);
    std::printf("  %sTravelling Salesman Problem%s — %s%u%s cities, "
                "%s%u%s agents\n",
        ansi::header, ansi::reset,
        ansi::stat_val, N_CITIES, ansi::reset,
        ansi::stat_val, POP, ansi::reset);
    std::printf("  Seed: %s%u%s   NN heuristic: %s%.1f%s → 2-opt: %s%.1f%s\n",
        ansi::stat_val, SEED, ansi::reset,
        ansi::stat_val, nn_length, ansi::reset,
        ansi::stat_val, nn_opt_length, ansi::reset);
    std::printf("  ACO: α=%s%.1f%s  β=%s%.1f%s  ρ=%s%.2f%s  Q=%s%.0f%s\n",
        ansi::stat_val, ACO_ALPHA, ansi::reset,
        ansi::stat_val, ACO_BETA, ansi::reset,
        ansi::stat_val, EVAP_RATE, ansi::reset,
        ansi::stat_val, Q_DEPOSIT, ansi::reset);
    std::printf("  Strategy: R^%u (tour-rank encoding)\n\n", N_CITIES);
    std::printf("  %sP(j) ∝ τ(i,j)^α · (1/d(i,j))^β%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sVicsek alignment = tour consensus  |  φ → 1 = "
                "route converged%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sMorphs: Explorer=random, Worker=greedy, "
                "Evaluator=2-opt, Coord=elite%s\n\n",
        ansi::dim, ansi::reset);
    std::printf("  %s═══════════════════════════════════════════════════"
                "═══════════════════════%s\n", ansi::border, ansi::reset);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ═══════════════════════════════════════════════════════════
    // ACO + Swarm loop
    // ═══════════════════════════════════════════════════════════

    uint32_t stagnation = 0;
    float prev_best = best_length;

    for (uint32_t round = 0; round < MAX_ROUNDS; ++round) {

        // Anneal noise: exploration → exploitation
        float prog = (float)round / (float)MAX_ROUNDS;
        float eta = ETA_HI * std::pow(ETA_LO / ETA_HI, prog);
        swarm.field().set_eta(eta);

        // ACO alpha/beta shift: more pheromone trust over time
        float alpha_t = ACO_ALPHA + prog * 0.5f;
        float beta_t  = ACO_BETA - prog * 0.5f;

        // ── Construct tours per agent ─────────────────────────
        float sum_length = 0;
        float round_best_len = 1e18f;
        uint32_t round_best_idx = 0;

        for (uint32_t a = 0; a < POP; ++a) {
            auto morph = swarm.scheduler().morph_of(a);

            if (morph == Morph::SwarmCoordinator) {
                // Coordinator: keep elite tour (mutate slightly)
                if (agent_tours[a].empty() || agent_lengths[a] > best_length * 1.1f)
                    agent_tours[a] = best_tour;
                // Small perturbation: swap two random cities
                if (agent_tours[a].size() >= 2) {
                    std::uniform_int_distribution<uint32_t> ci(0, N_CITIES - 1);
                    uint32_t p = ci(rng), q = ci(rng);
                    std::swap(agent_tours[a][p], agent_tours[a][q]);
                }
            } else if (morph == Morph::Explorer) {
                // Explorer: high randomness (low alpha, high noise)
                agent_tours[a] = construct_tour(N_CITIES, cities, tau,
                    alpha_t * 0.3f, beta_t * 0.5f, rng, 0);
            } else {
                // Worker: pheromone-greedy construction
                agent_tours[a] = construct_tour(N_CITIES, cities, tau,
                    alpha_t, beta_t, rng, 0);
            }

            // Evaluator: apply 2-opt
            if (morph == Morph::Evaluator) {
                for (int pass = 0; pass < 3; ++pass)
                    if (!two_opt_pass(agent_tours[a], cities)) break;
            }

            agent_lengths[a] = tour_length(agent_tours[a], cities);
            sum_length += agent_lengths[a];

            if (agent_lengths[a] < round_best_len) {
                round_best_len = agent_lengths[a];
                round_best_idx = a;
            }
        }

        // Update global best
        bool improved = false;
        if (round_best_len < best_length) {
            best_length = round_best_len;
            best_tour   = agent_tours[round_best_idx];
            // Apply 2-opt to global best
            for (int pass = 0; pass < 10; ++pass)
                if (!two_opt_pass(best_tour, cities)) break;
            best_length = tour_length(best_tour, cities);
            improved = true;
            stagnation = 0;
        } else {
            ++stagnation;
        }

        // ── Pheromone update ──────────────────────────────────
        // Evaporate
        for (uint32_t i = 0; i < N_CITIES; ++i)
            for (uint32_t j = 0; j < N_CITIES; ++j)
                tau[i][j] = std::max(TAU_MIN,
                    tau[i][j] * (1.0f - EVAP_RATE));

        // Deposit: all agents contribute proportional to 1/length
        for (uint32_t a = 0; a < POP; ++a) {
            float deposit = Q_DEPOSIT / agent_lengths[a];
            for (uint32_t s = 0; s < N_CITIES; ++s) {
                uint32_t ci = agent_tours[a][s];
                uint32_t cj = agent_tours[a][(s + 1) % N_CITIES];
                tau[ci][cj] = std::min(TAU_MAX, tau[ci][cj] + deposit);
                tau[cj][ci] = tau[ci][cj];
            }
        }

        // Elite deposit: extra pheromone on global best
        {
            float elite_dep = Q_DEPOSIT * 2.0f / best_length;
            for (uint32_t s = 0; s < N_CITIES; ++s) {
                uint32_t ci = best_tour[s];
                uint32_t cj = best_tour[(s + 1) % N_CITIES];
                tau[ci][cj] = std::min(TAU_MAX, tau[ci][cj] + elite_dep);
                tau[cj][ci] = tau[ci][cj];
            }
        }

        // Stagnation escape: reset pheromone if stuck
        if (stagnation > 25) {
            for (uint32_t i = 0; i < N_CITIES; ++i)
                for (uint32_t j = 0; j < N_CITIES; ++j)
                    tau[i][j] = TAU_INIT;
            stagnation = 0;
        }

        // ── Update swarm headings ─────────────────────────────
        float min_len = *std::min_element(agent_lengths.begin(),
                                          agent_lengths.end());
        float max_len = *std::max_element(agent_lengths.begin(),
                                          agent_lengths.end());

        for (uint32_t a = 0; a < POP; ++a) {
            swarm.field()[a].heading = tour_to_heading(agent_tours[a],
                                                       N_CITIES);
            float q = (max_len > min_len)
                ? 1.0f - (agent_lengths[a] - min_len) / (max_len - min_len)
                : 0.5f;
            swarm.field()[a].quality = q;
        }

        // ── Swarm tick ────────────────────────────────────────
        swarm.tick_cluster(cl);
        for (uint32_t a = 0; a < POP; ++a)
            swarm.field()[a].morph = swarm.scheduler().morph_of(a);

        // Engine pheromone deposit on best tour edges
        for (uint32_t s = 0; s < N_CITIES; ++s) {
            uint32_t ci = best_tour[s];
            uint32_t cj = best_tour[(s + 1) % N_CITIES];
            gr->deposit({TaskNodeId{ci + 1}, TaskNodeId{cj + 1}}, 0.1f);
        }

        // ── Diversity metric (unique edge count across all tours) ──
        std::map<uint64_t, uint32_t> edge_freq;
        for (uint32_t a = 0; a < POP; ++a) {
            for (uint32_t s = 0; s < N_CITIES; ++s) {
                uint32_t ci = agent_tours[a][s];
                uint32_t cj = agent_tours[a][(s + 1) % N_CITIES];
                uint64_t key = (uint64_t)std::min(ci, cj) << 32
                             | std::max(ci, cj);
                edge_freq[key]++;
            }
        }
        float diversity = (float)edge_freq.size()
            / (float)(N_CITIES * (N_CITIES - 1) / 2) * 100.0f;

        // ── Record history ────────────────────────────────────
        float avg_len = sum_length / (float)POP;
        best_hist.push_back(best_length);
        avg_hist.push_back(avg_len);
        phi_hist.push_back(cl.phi);
        div_hist.push_back(diversity);

        // ── Render ────────────────────────────────────────────
        std::printf("%s", ansi::home);
        std::printf("\n  %s═══ celer::necto::swarm ─── TSP Route Solver "
                    "══════════════════════════════%s\n\n",
                    ansi::border, ansi::reset);

        render_map(cities, best_tour, tau, MAP_W, MAP_H);
        std::printf("  %s%c%s=depot  %s%c%s=city  "
                    "%s─%s=best route  "
                    "%s·%s=pheromone trail%s\n\n",
            ansi::depot_clr, cities[0].label, ansi::reset,
            ansi::city_clr, cities[1].label, ansi::reset,
            ansi::best_clr, ansi::reset,
            ansi::phero_hi, ansi::reset,
            ansi::reset);

        // Stats
        const char* status_clr = improved ? ansi::improve : ansi::stagnate;
        std::printf("  Round %s%u%s/%u   Best: %s%.1f%s",
            ansi::stat_val, round, ansi::reset, MAX_ROUNDS,
            status_clr, best_length, ansi::reset);
        if (improved)
            std::printf("  %s▼ IMPROVED%s", ansi::improve, ansi::reset);
        else
            std::printf("  (stagnant %u)", stagnation);
        std::printf("\n");

        std::printf("  Avg: %s%.1f%s   NN baseline: %s%.1f%s"
                    "   Improvement: %s%.1f%%%s\n",
            ansi::stat_val, avg_len, ansi::reset,
            ansi::stat_val, nn_opt_length, ansi::reset,
            ansi::improve,
            (nn_opt_length - best_length) / nn_opt_length * 100.0f,
            ansi::reset);
        std::printf("  φ %s%.3f%s   η %s%.3f%s   diversity %s%.0f%%%s"
                    "   α %s%.2f%s   β %s%.2f%s\n",
            ansi::stat_val, cl.phi, ansi::reset,
            ansi::stat_val, eta, ansi::reset,
            ansi::stat_val, diversity, ansi::reset,
            ansi::stat_val, alpha_t, ansi::reset,
            ansi::stat_val, beta_t, ansi::reset);

        auto md = swarm.scheduler().distribution();
        std::printf("  %s●%s Rand %s%u%s  %s●%s Greedy %s%u%s  "
                    "%s●%s 2-opt %s%u%s  %s●%s Elite %s%u%s\n\n",
            ansi::explorer, ansi::reset, ansi::stat_val, md[0], ansi::reset,
            ansi::worker,   ansi::reset, ansi::stat_val, md[1], ansi::reset,
            ansi::evaluator,ansi::reset, ansi::stat_val, md[2], ansi::reset,
            ansi::coord,    ansi::reset, ansi::stat_val, md[3], ansi::reset);

        // Sparklines
        render_sparkline("best",  best_hist, 50, ansi::best_clr, true);
        render_sparkline("avg",   avg_hist,  50, ansi::route_clr, true);
        render_sparkline("φ(t)",  phi_hist,  50, ansi::ok);
        render_sparkline("div%",  div_hist,  50, ansi::warn, true);

        std::printf("\n  %sP(j) ∝ τ^α · (1/d)^β   MMAS bounds [%.2f, %.1f]%s\n",
            ansi::dim, TAU_MIN, TAU_MAX, ansi::reset);
        std::printf("  %sVicsek 1995 × ACO × Bonabeau 1996"
                    "   —   celer-mem / necto%s\n",
                    ansi::dim, ansi::reset);
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));

        // Early exit: converged and stable
        if (cl.phi > 0.98f && stagnation > 15 && round > 50) break;
    }

    // ═══════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════

    std::printf("%s", ansi::clear);
    std::printf("\n  %s═══ Route Optimization Complete "
                "═══════════════════════════════════════════%s\n\n",
                ansi::border, ansi::reset);

    std::printf("  %s── TSP Summary ──%s\n\n", ansi::header, ansi::reset);
    std::printf("  Cities:       %u\n", N_CITIES);
    std::printf("  Agents:       %u\n", POP);
    std::printf("  Rounds:       %zu\n", best_hist.size());
    std::printf("  Seed:         %u\n\n", SEED);
    std::printf("  NN heuristic: %.1f\n", nn_length);
    std::printf("  NN + 2-opt:   %.1f\n", nn_opt_length);
    std::printf("  %sSwarm best:   %.1f%s\n", ansi::best_clr, best_length,
        ansi::reset);
    std::printf("  Improvement:  %s%.1f%%%s over NN+2-opt baseline\n\n",
        ansi::improve,
        (nn_opt_length - best_length) / nn_opt_length * 100.0f,
        ansi::reset);

    // Print best tour
    std::printf("  Best route: ");
    for (uint32_t i = 0; i < best_tour.size(); ++i) {
        if (i > 0) std::printf("→");
        std::printf("%s%c%s",
            best_tour[i] == 0 ? ansi::depot_clr : ansi::city_clr,
            cities[best_tour[i]].label, ansi::reset);
    }
    std::printf("→%s%c%s\n\n",
        ansi::depot_clr, cities[best_tour[0]].label, ansi::reset);

    render_sparkline("best",  best_hist, 60, ansi::best_clr, true);
    render_sparkline("avg",   avg_hist,  60, ansi::route_clr, true);
    render_sparkline("φ(t)",  phi_hist,  60, ansi::ok);
    render_sparkline("div%",  div_hist,  60, ansi::warn, true);

    std::printf("\n  %sACO + Vicsek alignment + Bonabeau morphs + 2-opt%s\n",
        ansi::dim, ansi::reset);
    std::printf("  %sVicsek 1995 × ACO × Bonabeau 1996"
                "   —   celer-mem / necto%s\n\n",
                ansi::dim, ansi::reset);

    return 0;
}

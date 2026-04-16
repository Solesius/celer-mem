/// celer::necto::swarm — Live Terminal Visualizer
///
/// Renders a 2D swarm field with ANSI-colored directional arrows.
/// Watch 24 agents converge from random headings to aligned flocking,
/// with real-time morph re-assignment (Bonabeau response-threshold),
/// pheromone graph stats, and cluster order parameter φ.
///
/// Usage:  ./example_06_swarm_demo
/// Press Ctrl+C to exit early.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "celer/necto/swarm/swarm.hpp"

using namespace celer::necto;
using namespace celer::necto::swarm;

// ── ANSI escape helpers ──

namespace ansi {
    constexpr const char* clear      = "\033[2J\033[H";
    constexpr const char* reset      = "\033[0m";
    constexpr const char* bold       = "\033[1m";
    constexpr const char* dim        = "\033[2m";
    constexpr const char* explorer   = "\033[36m";
    constexpr const char* worker     = "\033[33m";
    constexpr const char* evaluator  = "\033[32m";
    constexpr const char* coord      = "\033[35m";
    constexpr const char* dormant    = "\033[90m";
    constexpr const char* header     = "\033[1;37m";
    constexpr const char* stat_key   = "\033[37m";
    constexpr const char* stat_val   = "\033[1;96m";
    constexpr const char* phi_high   = "\033[1;32m";
    constexpr const char* phi_mid    = "\033[1;33m";
    constexpr const char* phi_low    = "\033[1;31m";
    constexpr const char* bar_fill   = "\033[42m";
    constexpr const char* bar_empty  = "\033[100m";
    constexpr const char* border     = "\033[90m";
}

static auto morph_color(Morph m) -> const char* {
    switch (m) {
    case Morph::Explorer:         return ansi::explorer;
    case Morph::Worker:           return ansi::worker;
    case Morph::Evaluator:        return ansi::evaluator;
    case Morph::SwarmCoordinator: return ansi::coord;
    }
    return ansi::reset;
}

static auto morph_name(Morph m) -> const char* {
    switch (m) {
    case Morph::Explorer:         return "Explorer";
    case Morph::Worker:           return "Worker";
    case Morph::Evaluator:        return "Evaluator";
    case Morph::SwarmCoordinator: return "Coordinator";
    }
    return "?";
}

// ── Heading → arrow glyph (8 directions from R^2 unit vector) ──

static auto heading_arrow(const std::vector<float>& heading) -> const char* {
    if (heading.size() < 2) return "•";
    float x = heading[0], y = heading[1];
    float angle = std::atan2(y, x);  // radians, [-π, π]

    // Quantize to 8 directions
    // 0=→  1=↗  2=↑  3=↖  4=←  5=↙  6=↓  7=↘
    int octant = static_cast<int>(std::round(angle / (M_PI / 4.0))) % 8;
    if (octant < 0) octant += 8;

    static constexpr const char* arrows[] = {
        "→", "↗", "↑", "↖", "←", "↙", "↓", "↘"
    };
    return arrows[octant];
}

// ── Bar chart helper ──

static void render_bar(char* buf, std::size_t buf_sz, float value, int width) {
    int filled = static_cast<int>(value * static_cast<float>(width));
    if (filled > width) filled = width;

    int pos = 0;
    pos += std::snprintf(buf + pos, buf_sz - pos, "%s", ansi::bar_fill);
    for (int i = 0; i < filled; ++i)
        pos += std::snprintf(buf + pos, buf_sz - pos, " ");
    pos += std::snprintf(buf + pos, buf_sz - pos, "%s", ansi::bar_empty);
    for (int i = filled; i < width; ++i)
        pos += std::snprintf(buf + pos, buf_sz - pos, " ");
    std::snprintf(buf + pos, buf_sz - pos, "%s", ansi::reset);
}

// ── Grid layout ──

struct GridPos { int row; int col; };

static auto compute_grid(uint32_t count, int cols) -> std::vector<GridPos> {
    std::vector<GridPos> positions(count);
    for (uint32_t i = 0; i < count; ++i) {
        positions[i].row = static_cast<int>(i) / cols;
        positions[i].col = static_cast<int>(i) % cols;
    }
    return positions;
}

// ══════════════════════════════════════════════════════════════════
// DemoAgent — trivial actor; swarm behavior is in the field + scheduler
// ══════════════════════════════════════════════════════════════════

struct DemoAgent {
    uint32_t id{0};

    void on_receive(Envelope /*env*/, ActorContext& /*ctx*/) {
        // Swarm demo: behavior is driven by field alignment + morph scheduler,
        // not by message passing. This actor is a placeholder.
    }
};

// ══════════════════════════════════════════════════════════════════
// main — setup swarm, run tick loop with live rendering
// ══════════════════════════════════════════════════════════════════

int main() {
    constexpr uint32_t POPULATION     = 80;
    constexpr uint32_t STRAT_DIM        = 2;      // 2D for visualization
    constexpr float    RADIUS         = 2.0f;   // full connectivity
    constexpr float    NOISE_START    = 40.0f;  // high noise → disordered (φ ≈ 0.2)
    constexpr float    NOISE_END      = 3.0f;   // low noise → ordered (φ → 1.0)
    constexpr float    EVAP_RATE      = 0.05f;
    constexpr float    CONVERGE_PHI   = 0.95f;
    constexpr uint32_t SUSTAIN_WIN    = 10;
    constexpr uint32_t MAX_TICKS      = 200;
    constexpr int      GRID_COLS      = 6;
    constexpr int      FRAME_MS       = 80;
    constexpr uint32_t MIN_CONVERGE_TICK = 15; // warmup before convergence check

    // ── Morph configs (tuned for visible morph transitions) ──
    // Threshold tuning creates a 3-phase morph narrative:
    //   Chaos (φ<0.5):    Explorer + Coordinator compete
    //   Transition (φ~0.7): Worker + Evaluator rise as tasks progress
    //   Converged (φ>0.9):  Evaluator dominates
    std::vector<MorphConfig> morphs = {
        {"Explorer",    0.9f, 0.1f},   // hard to activate → shares chaos with Coord
        {"Worker",      0.3f, 0.1f},   // sensitive to InProgress tasks
        {"Evaluator",   0.3f, 0.1f},   // sensitive to Completed tasks
        {"Coordinator", 0.7f, 0.1f},   // activates when φ is low (chaos)
    };

    // ── Swarm config ──
    SwarmConfig cfg;
    cfg.population        = POPULATION;
    cfg.strategy_dim      = STRAT_DIM;
    cfg.alignment_radius  = RADIUS;
    cfg.noise_eta         = NOISE_START;
    cfg.evaporation_rate  = EVAP_RATE;
    cfg.convergence_phi   = CONVERGE_PHI;
    cfg.sustain_window    = SUSTAIN_WIN;
    cfg.max_rounds        = MAX_TICKS;
    cfg.premature_quality = 0.3f;
    cfg.noise_burst       = 0.5f;
    cfg.morphs            = morphs;
    cfg.channel_capacity  = 256;

    // ── Actor system + swarm ──
    ActorSystem system(256);

    // Seed from system clock so each run has different dynamics
    auto time_seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // Patch the field seed by constructing Swarm with a seeded config
    // (SwarmField takes seed= in its ctor, but Swarm auto-constructs it.
    // We rebuild the field post-construction with the real seed.)
    Swarm swarm(cfg, system);
    swarm.field() = SwarmField(POPULATION, STRAT_DIM, RADIUS, NOISE_START, time_seed);

    // Spawn prototype + clone_spawn population
    auto proto_ref = system.spawn<DemoAgent>("proto", DemoAgent{0});
    auto refs = system.spawn_n(proto_ref, POPULATION, "agent");

    // ── Create cluster with a root task + children ──
    TaskNode root;
    root.id          = TaskNodeId{0};
    root.description = "root-objective";
    root.status      = TaskStatus::Unexplored;

    auto& cluster = swarm.cluster_service().create_cluster(
        "alpha", root, EVAP_RATE, 1.0f, 1.0f, 2.0f, time_seed + 1);

    // Add child tasks to the pheromone graph
    auto* graph = swarm.cluster_service().graph_for(cluster);
    for (uint32_t t = 1; t <= 8; ++t) {
        TaskNode child;
        child.id          = TaskNodeId{t};
        child.description = "task-" + std::to_string(t);
        child.parent      = TaskNodeId{0};
        child.depth       = 1;
        child.status      = TaskStatus::Unexplored;
        graph->add_node(std::move(child));
        graph->ensure_edge(TaskNodeId{0}, TaskNodeId{t});
    }
    // Cross-edges for richer graph
    graph->ensure_edge(TaskNodeId{1}, TaskNodeId{3});
    graph->ensure_edge(TaskNodeId{2}, TaskNodeId{5});
    graph->ensure_edge(TaskNodeId{4}, TaskNodeId{7});

    // Recruit all agents into the cluster
    swarm.cluster_service().recruit(
        system, swarm.field(), cluster, swarm.field().agents(), POPULATION);

    // Seed some quality variation so morphs differ
    for (uint32_t i = 0; i < POPULATION; ++i) {
        swarm.field()[i].quality =
            0.1f + 0.8f * (static_cast<float>(i) / static_cast<float>(POPULATION));
    }

    // Deposit a pheromone trail so the graph has edges to display
    graph->deposit({TaskNodeId{0}, TaskNodeId{1}, TaskNodeId{3}}, 0.8f);
    graph->deposit({TaskNodeId{0}, TaskNodeId{2}, TaskNodeId{5}}, 0.5f);

    // Add per-agent threshold noise so different agents prefer different morphs
    {
        std::mt19937 rng(static_cast<uint32_t>(time_seed + 2));
        std::normal_distribution<float> noise(0.0f, 0.15f);
        auto& thresholds = swarm.scheduler().thresholds();
        for (uint32_t i = 0; i < POPULATION && i < thresholds.size(); ++i) {
            for (uint8_t c = 0; c < morph_count; ++c) {
                thresholds[i][c] = std::max(0.05f, thresholds[i][c] + noise(rng));
            }
        }
    }

    // ── Grid layout ──
    auto grid = compute_grid(POPULATION, GRID_COLS);
    int grid_rows = (static_cast<int>(POPULATION) + GRID_COLS - 1) / GRID_COLS;

    // ── Rendering loop ──
    bool converged = false;
    uint32_t converge_tick = 0;

    for (uint32_t tick = 0; tick < MAX_TICKS; ++tick) {
        // ── Annealing: exponential noise decay (chaos → order) ──
        float progress = static_cast<float>(tick) / static_cast<float>(MAX_TICKS);
        float current_eta = NOISE_START * std::pow(NOISE_END / NOISE_START, progress);
        swarm.field().set_eta(current_eta);

        // ── Tick the swarm ──
        swarm.tick_cluster(cluster);

        // Sync morph assignments from scheduler → field (for rendering)
        for (uint32_t i = 0; i < POPULATION; ++i) {
            swarm.field()[i].morph = swarm.scheduler().morph_of(i);
        }

        // Progress tasks through lifecycle for morph shifts
        if (tick == 15) graph->mark_status(TaskNodeId{1}, TaskStatus::InProgress);
        if (tick == 25) graph->mark_status(TaskNodeId{2}, TaskStatus::InProgress);
        if (tick == 40) graph->mark_status(TaskNodeId{1}, TaskStatus::Completed);
        if (tick == 50) graph->mark_status(TaskNodeId{3}, TaskStatus::InProgress);
        if (tick == 65) graph->mark_status(TaskNodeId{2}, TaskStatus::Completed);
        if (tick == 75) graph->mark_status(TaskNodeId{4}, TaskStatus::InProgress);
        if (tick == 90) graph->mark_status(TaskNodeId{3}, TaskStatus::Completed);
        if (tick == 100) graph->mark_status(TaskNodeId{5}, TaskStatus::InProgress);
        if (tick == 110) graph->mark_status(TaskNodeId{4}, TaskStatus::Completed);
        if (tick == 130) graph->mark_status(TaskNodeId{5}, TaskStatus::Completed);
        if (tick == 140) graph->mark_status(TaskNodeId{6}, TaskStatus::InProgress);
        if (tick == 160) graph->mark_status(TaskNodeId{6}, TaskStatus::Completed);

        // Check convergence (only after warmup)
        if (!converged && tick >= MIN_CONVERGE_TICK && cluster.phi > CONVERGE_PHI) {
            converge_tick = tick;
            converged = true;
        }

        // ── Take snapshot ──
        auto snap = swarm.snapshot();

        // ── Render frame ──
        std::printf("%s", ansi::clear);

        // Header
        std::printf("%s┌─── celer::necto::swarm ── Live Swarm Visualizer ────────────────────────────┐%s\n",
                    ansi::border, ansi::reset);
        std::printf("%s│%s                                                                              %s│%s\n",
                    ansi::border, ansi::reset, ansi::border, ansi::reset);

        // Agent grid + stats side panel
        int stats_col = GRID_COLS * 5 + 6;  // column where stats start

        for (int row = 0; row < std::max(grid_rows + 2, 18); ++row) {
            std::printf("%s│%s ", ansi::border, ansi::reset);

            // Left: agent grid
            if (row >= 1 && row <= grid_rows) {
                int grid_row = row - 1;
                std::printf("  ");
                for (int col = 0; col < GRID_COLS; ++col) {
                    int agent_idx = grid_row * GRID_COLS + col;
                    if (agent_idx < static_cast<int>(POPULATION)) {
                        const auto& agent = swarm.field()[agent_idx];
                        const char* clr = (agent.lifecycle == AgentLifecycle::Dormant)
                            ? ansi::dormant : morph_color(agent.morph);
                        std::printf(" %s%s%s %s", ansi::bold, clr,
                                    heading_arrow(agent.heading), ansi::reset);
                    } else {
                        std::printf("     ");
                    }
                }
                // Pad to stats column
                int used = 2 + GRID_COLS * 5;
                for (int p = used; p < stats_col; ++p) std::printf(" ");
            } else {
                for (int p = 0; p < stats_col; ++p) std::printf(" ");
            }

            // Right: stats panel
            char bar_buf[256];

            switch (row) {
            case 0:
                std::printf("%s┌─── Swarm Status ──────────────┐%s", ansi::border, ansi::reset);
                break;
            case 1: {
                const char* phi_clr = snap.phi > 0.9f ? ansi::phi_high
                    : snap.phi > 0.5f ? ansi::phi_mid : ansi::phi_low;
                std::printf("%s│%s %sTick%s  %s%-4u%s / %-4u   %sφ%s %s%.4f%s  %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_key, ansi::reset, ansi::stat_val, tick, ansi::reset, MAX_TICKS,
                    ansi::stat_key, ansi::reset, phi_clr, snap.phi, ansi::reset,
                    ansi::border, ansi::reset);
                break;
            }
            case 2:
                render_bar(bar_buf, sizeof(bar_buf), snap.phi, 20);
                std::printf("%s│%s  %s  %s%.0f%%%s               %s│%s",
                    ansi::border, ansi::reset,
                    bar_buf,
                    ansi::stat_val, snap.phi * 100.0f, ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 3:
                std::printf("%s│%s  %sNoise η%s %s%.3f%s              %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_key, ansi::reset,
                    ansi::stat_val, current_eta, ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 4:
                std::printf("%s│%s  %s── Morphs ──%s                  %s│%s",
                    ansi::border, ansi::reset, ansi::header, ansi::reset, ansi::border, ansi::reset);
                break;
            case 5:
                std::printf("%s│%s  %s●%s Explorer      %s%3u%s            %s│%s",
                    ansi::border, ansi::reset,
                    ansi::explorer, ansi::reset,
                    ansi::stat_val, snap.morph_distribution[0], ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 6:
                std::printf("%s│%s  %s●%s Worker        %s%3u%s            %s│%s",
                    ansi::border, ansi::reset,
                    ansi::worker, ansi::reset,
                    ansi::stat_val, snap.morph_distribution[1], ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 7:
                std::printf("%s│%s  %s●%s Evaluator     %s%3u%s            %s│%s",
                    ansi::border, ansi::reset,
                    ansi::evaluator, ansi::reset,
                    ansi::stat_val, snap.morph_distribution[2], ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 8:
                std::printf("%s│%s  %s●%s Coordinator   %s%3u%s            %s│%s",
                    ansi::border, ansi::reset,
                    ansi::coord, ansi::reset,
                    ansi::stat_val, snap.morph_distribution[3], ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 9:
                std::printf("%s│%s                                %s│%s",
                    ansi::border, ansi::reset, ansi::border, ansi::reset);
                break;
            case 10:
                std::printf("%s│%s  %s── Cluster ──%s                 %s│%s",
                    ansi::border, ansi::reset, ansi::header, ansi::reset, ansi::border, ansi::reset);
                break;
            case 11:
                std::printf("%s│%s  Name     %s%-12s%s         %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_val, cluster.name.c_str(), ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 12:
                std::printf("%s│%s  Members  %s%-4u%s  Edges %s%-4u%s    %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_val, static_cast<uint32_t>(cluster.members.size()), ansi::reset,
                    ansi::stat_val, graph ? graph->edge_count() : 0u, ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 13:
                std::printf("%s│%s  Sustain  %s%-4u%s / %-4u         %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_val, cluster.sustain_count, ansi::reset, SUSTAIN_WIN,
                    ansi::border, ansi::reset);
                break;
            case 14:
                std::printf("%s│%s  Quality  %s%.3f%s               %s│%s",
                    ansi::border, ansi::reset,
                    ansi::stat_val, snap.best_quality, ansi::reset,
                    ansi::border, ansi::reset);
                break;
            case 15:
                std::printf("%s│%s                                %s│%s",
                    ansi::border, ansi::reset, ansi::border, ansi::reset);
                break;
            case 16:
                if (converged) {
                    std::printf("%s│%s  %s✓ CONVERGED at tick %-4u%s       %s│%s",
                        ansi::border, ansi::reset,
                        ansi::phi_high, converge_tick, ansi::reset,
                        ansi::border, ansi::reset);
                } else {
                    std::printf("%s│%s  %s⟳ Aligning...%s                 %s│%s",
                        ansi::border, ansi::reset,
                        ansi::phi_mid, ansi::reset,
                        ansi::border, ansi::reset);
                }
                break;
            case 17:
                std::printf("%s└────────────────────────────────┘%s", ansi::border, ansi::reset);
                break;
            default:
                std::printf("                                ");
                break;
            }

            std::printf("\n");
        }

        // Footer
        std::printf("%s│%s                                                                              %s│%s\n",
                    ansi::border, ansi::reset, ansi::border, ansi::reset);

        // Legend
        std::printf("%s│%s  %sLegend:%s  %s%s→%s Explorer  %s%s→%s Worker  %s%s→%s Evaluator  %s%s→%s Coordinator",
            ansi::border, ansi::reset, ansi::dim, ansi::reset,
            ansi::explorer, ansi::bold, ansi::reset,
            ansi::worker, ansi::bold, ansi::reset,
            ansi::evaluator, ansi::bold, ansi::reset,
            ansi::coord, ansi::bold, ansi::reset);
        std::printf("          %s│%s\n", ansi::border, ansi::reset);

        std::printf("%s└──────────────────────────────────────────────────────────────────────────────┘%s\n",
                    ansi::border, ansi::reset);

        std::printf("\n  %sVicsek 1995 × Bonabeau 1996 × Dorigo 1996%s  —  celer-mem / necto\n", ansi::dim, ansi::reset);

        // ── Frame delay ──
        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));

        // Run a bit longer after convergence so user can see the result
        if (converged && tick > converge_tick + 40) break;
    }

    // Final summary
    std::printf("\n  %s── Summary ──%s\n", ansi::header, ansi::reset);
    std::printf("  Population:  %u agents\n", POPULATION);
    std::printf("  Dimensions:  %u (2D visualization)\n", STRAT_DIM);
    std::printf("  Final φ:     %.4f\n", cluster.phi);
    std::printf("  Converged:   %s", converged ? "yes" : "no");
    if (converged) std::printf(" (tick %u)", converge_tick);
    std::printf("\n  Ticks run:   %u\n", converged ? converge_tick + 41 : MAX_TICKS);
    std::printf("  Edges:       %u\n", graph ? graph->edge_count() : 0u);

    auto dist = swarm.scheduler().distribution();
    std::printf("  Morphs:      E=%u  W=%u  V=%u  C=%u\n",
        dist[0], dist[1], dist[2], dist[3]);

    std::printf("\n");
    return 0;
}

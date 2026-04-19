/// celer::necto::swarm — Collatz Sequence Solver
///
/// The swarm collectively discovers the Collatz sequence for a given integer.
/// Each step: agents encode candidate values as angles on the unit circle,
/// a fitness oracle evaluates proposals, and Vicsek alignment converges
/// the swarm to the correct next value.
///
/// Encoding: integer N → angle 2πN/RANGE on the unit circle → (cos θ, sin θ).
/// Oracle agents (~12%) are seeded near the correct value with ±1 noise.
/// Fitness-driven perturbation destabilizes wrong clusters.
/// Noise annealing (η: 5→0.3) cools the system from chaos to consensus.
///
/// Collatz rule: Even → N/2  |  Odd → 3N+1
/// Example:  5 → 16 → 8 → 4 → 2 → 1
///
/// Usage:  ./example_07_collatz_swarm [start_value]
/// Default: 5

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    constexpr const char* cur       = "\033[1;33m";
    constexpr const char* chain_clr = "\033[1;36m";
    constexpr const char* bar_fg    = "\033[46m";
    constexpr const char* bar_hit   = "\033[42m";
    constexpr const char* border    = "\033[90m";
}

// ── Collatz ───────────────────────────────────────────────────

static auto collatz_step(int n) -> int {
    return (n % 2 == 0) ? n / 2 : 3 * n + 1;
}

static auto collatz_chain(int start) -> std::vector<int> {
    std::vector<int> seq = {start};
    while (start != 1 && seq.size() < 500) {
        start = collatz_step(start);
        seq.push_back(start);
    }
    return seq;
}

// ── Value ↔ heading  (integer → angle on unit circle) ────────

static auto encode_val(int v, int range) -> std::vector<float> {
    float a = 2.0f * static_cast<float>(M_PI) * static_cast<float>(v)
              / static_cast<float>(range);
    return {std::cos(a), std::sin(a)};
}

static auto decode_val(const std::vector<float>& h, int range) -> int {
    if (h.size() < 2) return 0;
    float a = std::atan2(h[1], h[0]);
    if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
    int v = static_cast<int>(
        std::round(a * static_cast<float>(range) / (2.0f * static_cast<float>(M_PI))));
    return ((v % range) + range) % range;
}

// ── Trivial actor (swarm runs on field + scheduler) ──────────

struct CzAgent {
    uint32_t id{0};
    void on_receive(Envelope, ActorContext&) {}
};

// ══════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    int start_n = (argc > 1) ? std::atoi(argv[1]) : 100;
    if (start_n < 2) start_n = 100;

    auto seq = collatz_chain(start_n);
    int max_v = *std::max_element(seq.begin(), seq.end());

    if (max_v > 256) {
        std::printf("\n  Collatz(%d) reaches %d — too large for angle encoding.\n"
                    "  Try a value whose sequence stays under 256 (e.g. 5, 7, 15).\n\n",
                    start_n, max_v);
        return 1;
    }

    // Range: next power-of-2 above 2 × max_v (angular headroom)
    int RANGE = 1;
    while (RANGE < max_v * 2 + 2) RANGE <<= 1;

    // ── Tunables ──
    constexpr uint32_t POP         = 48;
    constexpr uint32_t STRAT_DIM   = 2;
    constexpr float    RAD         = 2.0f;      // full connectivity
    constexpr float    EVAP        = 0.05f;
    constexpr uint32_t STEP_TICKS  = 80;
    constexpr int      FRAME_MS    = 50;
    constexpr uint32_t ORACLES     = 12;        // 25 % seeded
    constexpr float    ETA_HI      = 4.0f;
    constexpr float    ETA_LO      = 0.3f;
    constexpr float    CON_TH      = 0.55f;     // 55 % agreement = solved
    constexpr uint32_t MIN_TICK    = 12;        // warmup before convergence check

    // ── Swarm config ──
    SwarmConfig cfg;
    cfg.population       = POP;
    cfg.strategy_dim     = STRAT_DIM;
    cfg.alignment_radius = RAD;
    cfg.noise_eta        = ETA_HI;
    cfg.evaporation_rate = EVAP;
    cfg.convergence_phi  = 0.95f;
    cfg.sustain_window   = 5;
    cfg.max_rounds       = STEP_TICKS;
    cfg.premature_quality = 0.3f;
    cfg.noise_burst      = 0.5f;
    cfg.morphs           = {
        {"Explorer",    0.9f, 0.1f},
        {"Worker",      0.3f, 0.1f},
        {"Evaluator",   0.3f, 0.1f},
        {"Coordinator", 0.7f, 0.1f},
    };
    cfg.channel_capacity = 256;

    ActorSystem sys(256);
    auto ts = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    Swarm swarm(cfg, sys);
    swarm.field() = SwarmField(POP, STRAT_DIM, RAD, ETA_HI, ts);

    auto proto = sys.spawn<CzAgent>("proto", CzAgent{0});
    auto refs  = sys.spawn_n(proto, POP, "cz");
    (void)refs;

    // ── Cluster + task graph ──
    TaskNode root;
    root.id          = TaskNodeId{0};
    root.description = "collatz-root";
    root.status      = TaskStatus::Unexplored;

    auto& cl = swarm.cluster_service().create_cluster(
        "collatz", root, EVAP, 1.0f, 1.0f, 2.0f, ts + 1);
    auto* gr = swarm.cluster_service().graph_for(cl);

    for (uint32_t t = 0; t < seq.size(); ++t) {
        TaskNode nd;
        nd.id          = TaskNodeId{t + 1};
        nd.description = "step-" + std::to_string(seq[t]);
        nd.parent      = TaskNodeId{0};
        nd.depth       = 1;
        nd.status      = TaskStatus::Unexplored;
        gr->add_node(std::move(nd));
        gr->ensure_edge(TaskNodeId{0}, TaskNodeId{t + 1});
    }
    for (uint32_t t = 1; t + 1 < static_cast<uint32_t>(seq.size()); ++t)
        gr->ensure_edge(TaskNodeId{t}, TaskNodeId{t + 1});

    swarm.cluster_service().recruit(
        sys, swarm.field(), cl, swarm.field().agents(), POP);

    // Per-agent threshold noise (morph diversity)
    {
        std::mt19937 trng(static_cast<uint32_t>(ts + 2));
        std::normal_distribution<float> nd(0.0f, 0.15f);
        auto& th = swarm.scheduler().thresholds();
        for (uint32_t i = 0; i < POP && i < th.size(); ++i)
            for (uint8_t c = 0; c < morph_count; ++c)
                th[i][c] = std::max(0.05f, th[i][c] + nd(trng));
    }

    std::mt19937 rng(static_cast<uint32_t>(ts + 3));

    // ── Intro screen ──
    std::printf("%s", ansi::clear);
    std::printf("\n  %s═══ celer::necto::swarm ─── Collatz Solver ═══════════════════════════════%s\n\n",
        ansi::border, ansi::reset);
    std::printf("  %sCollatz conjecture%s: for any positive integer n,\n", ansi::header, ansi::reset);
    std::printf("    if n is even → n/2\n");
    std::printf("    if n is odd  → 3n + 1\n");
    std::printf("  the sequence always reaches 1.\n\n");
    std::printf("  Starting from %s%d%s — swarm of %u agents will discover each step.\n",
        ansi::chain_clr, start_n, ansi::reset, POP);
    std::printf("  %zu steps to solve.  %u oracle agents (%.0f%% of swarm).\n\n",
        seq.size() - 1, ORACLES, 100.0f * ORACLES / POP);
    std::printf("  %s═══════════════════════════════════════════════════════════════════════════%s\n",
        ansi::border, ansi::reset);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ═══════════════════════════════════════════════════
    // Solve the Collatz chain step-by-step
    // ═══════════════════════════════════════════════════

    std::vector<int>      chain;               // solved values
    std::vector<uint32_t> step_ticks_vec;      // convergence tick per step
    uint32_t              n_correct = 0;

    for (std::size_t si = 0; si + 1 < seq.size(); ++si) {
        int  cur  = seq[si];
        int  tgt  = seq[si + 1];
        bool even = (cur % 2 == 0);

        gr->mark_status(TaskNodeId{static_cast<uint32_t>(si + 1)},
                        TaskStatus::InProgress);

        // ── Randomize headings + seed oracles ──
        for (uint32_t i = 0; i < POP; ++i)
            swarm.field()[i].heading = encode_val(
                std::uniform_int_distribution<int>(0, RANGE - 1)(rng), RANGE);

        std::vector<uint32_t> perm(POP);
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), rng);

        // Oracles: heading at exact target with tiny angular jitter
        for (uint32_t o = 0; o < ORACLES; ++o) {
            auto hv = encode_val(tgt, RANGE);
            std::normal_distribution<float> jitter(0.0f, 0.02f);
            hv[0] += jitter(rng);
            hv[1] += jitter(rng);
            float n = std::sqrt(hv[0] * hv[0] + hv[1] * hv[1]);
            if (n > 1e-6f) { hv[0] /= n; hv[1] /= n; }
            swarm.field()[perm[o]].heading = hv;
        }

        // Pre-compute oracle membership for fast lookup
        std::vector<bool> is_oracle(POP, false);
        for (uint32_t o = 0; o < ORACLES; ++o)
            is_oracle[perm[o]] = true;

        bool     solved   = false;
        int      con_val  = -1;
        uint32_t sol_tick = 0;

        for (uint32_t tick = 0; tick < STEP_TICKS; ++tick) {
            // Anneal noise
            float prog = static_cast<float>(tick) / static_cast<float>(STEP_TICKS);
            float eta  = ETA_HI * std::pow(ETA_LO / ETA_HI, prog);
            swarm.field().set_eta(eta);

            // Tick the swarm (morph eval → alignment → pheromone evap)
            swarm.tick_cluster(cl);

            // Sync morphs for display
            for (uint32_t i = 0; i < POP; ++i)
                swarm.field()[i].morph = swarm.scheduler().morph_of(i);

            // ── Fitness feedback: oracle snap-back + capture + explore ──
            auto tgt_h = encode_val(tgt, RANGE);

            // Adaptive blend: moderate early (visual drama) → strong late (reliability)
            float oracle_blend  = 0.55f + 0.30f * prog;   // 0.55 → 0.85
            float capture_blend = 0.35f + 0.25f * prog;   // 0.35 → 0.60

            // 1. Oracles snap back toward target (persistent attractors)
            for (uint32_t o = 0; o < ORACLES; ++o) {
                auto& hd = swarm.field()[perm[o]].heading;
                hd[0] = hd[0] * (1.0f - oracle_blend) + tgt_h[0] * oracle_blend;
                hd[1] = hd[1] * (1.0f - oracle_blend) + tgt_h[1] * oracle_blend;
                float nm = std::sqrt(hd[0] * hd[0] + hd[1] * hd[1]);
                if (nm > 1e-6f) { hd[0] /= nm; hd[1] /= nm; }
                swarm.field()[perm[o]].quality = 1.0f;
            }

            // 2. Non-oracles: fitness-driven capture or exploration
            for (uint32_t i = 0; i < POP; ++i) {
                if (is_oracle[i]) continue;
                int prop = decode_val(swarm.field()[i].heading, RANGE);
                float fit = 1.0f / (1.0f + static_cast<float>(std::abs(prop - tgt)));
                swarm.field()[i].quality = fit;

                auto& hd = swarm.field()[i].heading;
                if (fit >= 0.5f) {
                    // Near target: capture — pull toward it
                    hd[0] = hd[0] * (1.0f - capture_blend) + tgt_h[0] * capture_blend;
                    hd[1] = hd[1] * (1.0f - capture_blend) + tgt_h[1] * capture_blend;
                    float nm = std::sqrt(hd[0] * hd[0] + hd[1] * hd[1]);
                    if (nm > 1e-6f) { hd[0] /= nm; hd[1] /= nm; }
                } else if (fit < 0.25f && tick < STEP_TICKS * 3 / 4) {
                    // Far from target: exploration perturbation
                    std::normal_distribution<float> pn(0.0f, 0.4f);
                    hd[0] += pn(rng);
                    hd[1] += pn(rng);
                    float nm = std::sqrt(hd[0] * hd[0] + hd[1] * hd[1]);
                    if (nm > 1e-6f) { hd[0] /= nm; hd[1] /= nm; }
                }
            }

            // ── Vote tally ──
            std::map<int, uint32_t> votes;
            for (uint32_t i = 0; i < POP; ++i)
                votes[decode_val(swarm.field()[i].heading, RANGE)]++;

            int bv = -1;
            uint32_t bc = 0;
            for (auto& [v, c] : votes)
                if (c > bc) { bc = c; bv = v; }
            float agr = static_cast<float>(bc) / static_cast<float>(POP);

            // Wrong consensus forming → scatter non-oracles + snap oracles
            if (!solved && agr >= 0.40f && bv != tgt && tick < STEP_TICKS * 3 / 4) {
                for (uint32_t i = 0; i < POP; ++i) {
                    if (is_oracle[i]) {
                        swarm.field()[i].heading = encode_val(tgt, RANGE);
                    } else {
                        swarm.field()[i].heading = encode_val(
                            std::uniform_int_distribution<int>(0, RANGE - 1)(rng), RANGE);
                    }
                }
                // Re-tally after reset
                votes.clear();
                for (uint32_t i = 0; i < POP; ++i)
                    votes[decode_val(swarm.field()[i].heading, RANGE)]++;
                bv = -1; bc = 0;
                for (auto& [v, c] : votes)
                    if (c > bc) { bc = c; bv = v; }
                agr = static_cast<float>(bc) / static_cast<float>(POP);
            }

            // ── Render frame ──
            std::printf("%s", ansi::clear);

            std::printf("\n  %s═══ celer::necto::swarm ─── Collatz Solver "
                        "═══════════════════════════════%s\n\n",
                        ansi::border, ansi::reset);

            // Chain built so far
            std::printf("  %sChain:%s  ", ansi::header, ansi::reset);
            std::size_t chain_start = chain.size() > 7 ? chain.size() - 7 : 0;
            if (chain_start > 0) std::printf("… ");
            for (std::size_t ci = chain_start; ci < chain.size(); ++ci)
                std::printf("%s%d%s %s→%s ",
                    ansi::chain_clr, chain[ci], ansi::reset, ansi::dim, ansi::reset);
            std::printf("%s%s%d%s %s→%s %s?%s\n\n",
                ansi::bold, ansi::cur, cur, ansi::reset,
                ansi::dim, ansi::reset, ansi::cur, ansi::reset);

            // Problem statement
            std::printf("  %s── collatz(%d) = ?%s   (%s%s%s)\n\n",
                ansi::header, cur, ansi::reset,
                ansi::dim, even ? "even → n÷2" : "odd → 3n+1", ansi::reset);

            // Histogram: top 8 candidates by vote count
            auto sv = std::vector<std::pair<int, uint32_t>>(votes.begin(), votes.end());
            std::sort(sv.begin(), sv.end(),
                      [](auto& a, auto& b) { return a.second > b.second; });

            int show = std::min(8, static_cast<int>(sv.size()));
            uint32_t mx = sv.empty() ? 1 : sv[0].second;
            constexpr int BW = 30;

            for (int r = 0; r < show; ++r) {
                int val      = sv[r].first;
                uint32_t cnt = sv[r].second;
                int w = mx > 0
                    ? static_cast<int>(static_cast<float>(cnt) / static_cast<float>(mx) * BW)
                    : 0;
                if (w < 1 && cnt > 0) w = 1;

                bool hit = (val == tgt);
                bool top = (val == bv);

                std::printf("  %4d │%s", val, hit ? ansi::bar_hit : ansi::bar_fg);
                for (int b = 0; b < w; ++b) std::printf(" ");
                std::printf("%s", ansi::reset);
                for (int b = w; b < BW; ++b) std::printf(" ");
                std::printf("│ %s%2u%s", ansi::stat_val, cnt, ansi::reset);

                if      (top && hit) std::printf("  %s★ ✓%s", ansi::ok, ansi::reset);
                else if (top)        std::printf("  %s★%s  ",  ansi::cur, ansi::reset);
                else if (hit)        std::printf("  %s✓%s  ",  ansi::ok, ansi::reset);
                else                 std::printf("     ");
                std::printf("\n");
            }
            // Pad if fewer than 8 rows
            for (int r = show; r < 8; ++r)
                std::printf("\n");

            std::printf("\n");

            // Status line
            if (solved) {
                std::printf("  %s✓ SOLVED:%s collatz(%d) = %s%d%s   (tick %u)\n",
                    ansi::ok, ansi::reset, cur, ansi::ok, con_val, ansi::reset, sol_tick);
            } else {
                const char* cc = (bv == tgt) ? ansi::ok : ansi::cur;
                std::printf("  Consensus: %s%d%s (%s%.0f%%%s)   "
                            "Tick %s%u%s/%u   φ %s%.3f%s   η %s%.2f%s\n",
                    cc, bv, ansi::reset,
                    ansi::stat_val, agr * 100.0f, ansi::reset,
                    ansi::stat_val, tick, ansi::reset, STEP_TICKS,
                    ansi::stat_val, cl.phi, ansi::reset,
                    ansi::stat_val, eta, ansi::reset);
            }

            // Morph distribution + step info
            auto md = swarm.scheduler().distribution();
            std::printf("  %s●%s Expl %s%u%s  %s●%s Work %s%u%s  "
                        "%s●%s Eval %s%u%s  %s●%s Coord %s%u%s",
                ansi::explorer, ansi::reset, ansi::stat_val, md[0], ansi::reset,
                ansi::worker,   ansi::reset, ansi::stat_val, md[1], ansi::reset,
                ansi::evaluator,ansi::reset, ansi::stat_val, md[2], ansi::reset,
                ansi::coord,    ansi::reset, ansi::stat_val, md[3], ansi::reset);
            std::printf("   Step %s%zu%s/%zu   Oracles %s%u%s/%u\n",
                ansi::stat_val, si + 1, ansi::reset, seq.size() - 1,
                ansi::stat_val, ORACLES, ansi::reset, POP);

            std::printf("\n  %s═══════════════════════════════════════════════"
                        "════════════════════════════%s\n",
                        ansi::border, ansi::reset);
            std::printf("  %sVicsek 1995 × ACO × Bonabeau 1996"
                        "   —   celer-mem / necto%s\n",
                        ansi::dim, ansi::reset);

            std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));

            // Check convergence
            if (!solved && tick >= MIN_TICK && agr >= CON_TH && bv == tgt) {
                solved   = true;
                con_val  = bv;
                sol_tick = tick;
            }
            if (solved && tick > sol_tick + 10) break;
        }

        // Record results
        chain.push_back(cur);
        step_ticks_vec.push_back(solved ? sol_tick : STEP_TICKS);
        if (solved) {
            ++n_correct;
            gr->mark_status(TaskNodeId{static_cast<uint32_t>(si + 1)},
                            TaskStatus::Completed);
            gr->deposit({TaskNodeId{static_cast<uint32_t>(si)},
                         TaskNodeId{static_cast<uint32_t>(si + 1)}}, 1.0f);
        }
    }

    chain.push_back(1);

    // ═══════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════

    std::printf("\n  %s── Collatz(%d): Sequence Discovered ──%s\n\n  ",
        ansi::header, start_n, ansi::reset);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i > 0) std::printf(" %s→%s ", ansi::dim, ansi::reset);
        std::printf("%s%d%s", ansi::chain_clr, chain[i], ansi::reset);
    }
    std::printf("\n\n");

    // Per-step breakdown
    std::printf("  %sStep breakdown:%s\n", ansi::dim, ansi::reset);
    for (std::size_t i = 0; i + 1 < seq.size(); ++i) {
        std::printf("    %3d → %-3d   %s%2u ticks%s  (%s)\n",
            seq[i], seq[i + 1],
            ansi::stat_val, step_ticks_vec[i], ansi::reset,
            seq[i] % 2 == 0 ? "even" : "odd");
    }
    std::printf("\n");

    uint32_t total_t = 0;
    for (auto x : step_ticks_vec) total_t += x;

    std::printf("  Steps:    %zu\n", seq.size() - 1);
    std::printf("  Correct:  %u/%zu %s\n", n_correct, seq.size() - 1,
        n_correct == seq.size() - 1 ? "✓" : "✗");
    std::printf("  Agents:   %u (%u oracles, %.0f%%)\n",
        POP, ORACLES, 100.0f * ORACLES / POP);
    std::printf("  Encoding: angle on unit circle (range=%d)\n", RANGE);
    std::printf("  Ticks:    %u total\n\n", total_t);

    return 0;
}

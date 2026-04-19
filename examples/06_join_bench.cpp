/// examples/06_join_bench.cpp — 100k × 100k cross-store join benchmark.
///
/// Pipeline:
///   1. Seed a "users"  table (RocksDB) with 100k records.
///   2. Seed a "posts"  table (SQLite) with 100k records, each pointing
///      at a user_id.
///   3. Stream posts → join against users on user_id → materialize the
///      view rows back into a "posts_with_user" target (RocksDB).
///   4. Print throughput + per-stage timings + planner choice.
///
/// Build: configure with -DCELER_BUILD_EXAMPLES=ON, then
///        ./build/cmake/example_06_join_bench
///
/// Expected (release build, modern laptop): full pipeline well under 1s.

#include <celer/celer.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs  = std::filesystem;
namespace mat = celer::materialization;

using Clock = std::chrono::steady_clock;

static auto ms_since(Clock::time_point t0) -> double {
    auto d = Clock::now() - t0;
    return std::chrono::duration<double, std::milli>(d).count();
}

static auto stage(const char* label, double ms, std::size_t rows) -> void {
    const double rps = rows > 0 ? (rows / (ms / 1000.0)) : 0.0;
    std::cout << "  " << std::left << std::setw(28) << label
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) << ms << " ms  "
              << std::setw(12) << rows << " rows  "
              << std::setw(12) << static_cast<long long>(rps) << " rows/s\n";
}

int main() {
    constexpr std::size_t kUsers = 100'000;
    constexpr std::size_t kPosts = 100'000;

    const auto users_path = fs::temp_directory_path() / "celer_bench_users";
    const auto posts_path = fs::temp_directory_path() / "celer_bench_posts.db";
    const auto view_path  = fs::temp_directory_path() / "celer_bench_view";
    fs::remove_all(users_path);
    fs::remove_all(posts_path);
    fs::remove_all(view_path);

    std::cout << "═══════════════════════════════════════════════════════════════\n"
              << "  celer-mem RFC-005 join benchmark — " << kPosts << " × " << kUsers << "\n"
              << "═══════════════════════════════════════════════════════════════\n";

    // ── 1. Open three stores: users (Rocks), posts (SQLite), view (Rocks) ──
    auto users_store = celer::build_tree(
        celer::backends::rocksdb::factory({.path = users_path.string()}),
        std::vector<celer::TableDescriptor>{{"users", "by_id"}});
    if (!users_store) { std::cerr << "users open: " << users_store.error().message << "\n"; return 1; }

    auto posts_store = celer::build_tree(
        celer::backends::sqlite::factory({.path = posts_path.string()}),
        std::vector<celer::TableDescriptor>{{"posts", "by_id"}});
    if (!posts_store) { std::cerr << "posts open: " << posts_store.error().message << "\n"; return 1; }

    auto view_store = celer::build_tree(
        celer::backends::rocksdb::factory({.path = view_path.string()}),
        std::vector<celer::TableDescriptor>{{"materialized", "posts_with_user"}});
    if (!view_store) { std::cerr << "view open: " << view_store.error().message << "\n"; return 1; }

    celer::Store us{std::move(*users_store), celer::ResourceStack{}};
    celer::Store ps{std::move(*posts_store), celer::ResourceStack{}};
    celer::Store vs{std::move(*view_store),  celer::ResourceStack{}};

    auto users_tbl = us.db("users")->table("by_id");
    auto posts_tbl = ps.db("posts")->table("by_id");
    auto view_tbl  = vs.db("materialized")->table("posts_with_user");

    mat::StoreRef<std::string> users{*users_tbl};
    mat::StoreRef<std::string> posts{*posts_tbl};
    mat::StoreRef<std::string> view {*view_tbl};

    // ── 2. Seed users ──
    {
        auto t0 = Clock::now();
        std::vector<std::pair<std::string, std::string>> rows;
        rows.reserve(kUsers);
        for (std::size_t i = 0; i < kUsers; ++i) {
            // value = "alice_<i>|us-east-1" — small payload, realistic shape
            rows.emplace_back("u" + std::to_string(i),
                              "alice_" + std::to_string(i) + "|us-east-1");
        }
        if (auto r = users.put_many(rows); !r) {
            std::cerr << "seed users: " << r.error().message << "\n"; return 1;
        }
        stage("seed users (RocksDB)", ms_since(t0), kUsers);
    }

    // ── 3. Seed posts (each post → random user_id) ──
    {
        auto t0 = Clock::now();
        std::mt19937_64 rng{0xC0FFEE};
        std::uniform_int_distribution<std::size_t> uid_dist{0, kUsers - 1};

        std::vector<std::pair<std::string, std::string>> rows;
        rows.reserve(kPosts);
        for (std::size_t i = 0; i < kPosts; ++i) {
            // value = "u<uid>|<title>" — first segment is the join key
            const auto uid = uid_dist(rng);
            rows.emplace_back("p" + std::to_string(i),
                              "u" + std::to_string(uid) + "|post-title-" + std::to_string(i));
        }
        if (auto r = posts.put_many(rows); !r) {
            std::cerr << "seed posts: " << r.error().message << "\n"; return 1;
        }
        stage("seed posts (SQLite)", ms_since(t0), kPosts);
    }

    // ── 4. Print planner choice for the join ──
    {
        mat::PlannerInputs in;
        in.left_caps  = posts.capabilities();
        in.right_caps = users.capabilities();
        auto plan = mat::JoinPlanner::plan(in);
        const char* name = "unknown";
        switch (plan.strategy) {
            case mat::JoinStrategy::Auto:                  name = "Auto"; break;
            case mat::JoinStrategy::BatchIndexNestedLoop:  name = "BatchIndexNestedLoop"; break;
            case mat::JoinStrategy::IndexNestedLoop:       name = "IndexNestedLoop"; break;
            case mat::JoinStrategy::HashJoin:              name = "HashJoin"; break;
            case mat::JoinStrategy::MergeJoin:             name = "MergeJoin"; break;
            case mat::JoinStrategy::NestedLoop:            name = "NestedLoop"; break;
        }
        std::cout << "\n  planner → " << name
                  << "  (batch_size=" << plan.batch_size << ")\n\n";
    }

    // ── 5. Stream posts → join with users → collect ──
    std::size_t join_rows = 0;
    double join_ms = 0.0;
    {
        auto src = mat::stream_from(posts);
        if (!src) { std::cerr << "scan posts: " << src.error().message << "\n"; return 1; }

        auto t0 = Clock::now();
        auto j = mat::join<std::string, std::string>(
            std::move(*src), users,
            [](const mat::Record<std::string>& r) -> celer::Result<std::string> {
                // user_id is the prefix up to the first '|'
                const auto pipe = r.value.find('|');
                if (pipe == std::string::npos) {
                    return std::unexpected(celer::Error{"BadShape", "missing '|' in post value"});
                }
                return r.value.substr(0, pipe);
            },
            mat::JoinOptions{
                .kind        = mat::JoinKind::Inner,
                .batch_size  = 1024,
            });
        if (!j) { std::cerr << "join: " << j.error().message << "\n"; return 1; }

        auto out = std::move(*j).collect();
        if (!out) { std::cerr << "collect: " << out.error().message << "\n"; return 1; }
        join_ms   = ms_since(t0);
        join_rows = out->size();
        stage("join (collect)", join_ms, join_rows);
    }

    // ── 6. Stream + materialize into view ──
    {
        auto src = mat::stream_from(posts);
        if (!src) { std::cerr << "scan posts: " << src.error().message << "\n"; return 1; }

        auto t0 = Clock::now();
        auto j = mat::join<std::string, std::string>(
            std::move(*src), users,
            [](const mat::Record<std::string>& r) -> celer::Result<std::string> {
                const auto pipe = r.value.find('|');
                if (pipe == std::string::npos) {
                    return std::unexpected(celer::Error{"BadShape", "missing '|'"});
                }
                return r.value.substr(0, pipe);
            },
            mat::JoinOptions{.kind = mat::JoinKind::Inner, .batch_size = 1024});
        if (!j) { std::cerr << "join: " << j.error().message << "\n"; return 1; }

        // The Joined<L,R> stream has key = right key (user_id). For the view,
        // we want to key by post_id and store the joined value as
        // "<title>|<user_meta>". Map then materialize.
        auto mapped = std::move(*j).map(
            [](const mat::Joined<std::string, std::string>& jj) -> std::string {
                // jj.left  = full posts value ("u<uid>|<title>")
                // jj.right = users value     ("alice_<i>|<region>")
                const auto pipe = jj.left.find('|');
                std::string title = pipe == std::string::npos
                                       ? jj.left
                                       : jj.left.substr(pipe + 1);
                return title + " :: " + jj.right.value_or("?");
            });

        // Materialize. Key by the inherited post id (Joined's `key` is the
        // right-side join key, so we need to remap to the original post id).
        // For this benchmark we key by the join key directly to keep the
        // pipeline pure-streaming. (A real app would carry the post_id
        // through Joined.key via a custom extractor.)
        auto r = mat::materialize(
            std::move(mapped), view,
            [](const mat::Record<std::string>& r) -> celer::Result<std::string> {
                return r.key;
            },
            mat::MaterializeOptions{
                .mode             = mat::MaterializeMode::Upsert,
                .flush_batch_size = 4096,
            });
        if (!r) { std::cerr << "materialize: " << r.error().message << "\n"; return 1; }

        const double mat_ms = ms_since(t0);
        stage("join + materialize → view", mat_ms, r->rows_written);
        std::cout << "    bytes_written = " << r->metrics.bytes_written
                  << "  flushes = "         << r->metrics.flushes << "\n";
    }

    // ── 7. Summary ──
    std::cout << "\n  ───────────────────────────────────────────────────────────\n"
              << "  TOTAL join throughput: "
              << static_cast<long long>(join_rows / (join_ms / 1000.0))
              << " rows/s   (" << join_rows << " rows in " << std::fixed
              << std::setprecision(2) << join_ms << " ms)\n";

    fs::remove_all(users_path);
    fs::remove_all(posts_path);
    fs::remove_all(view_path);
    return 0;
}

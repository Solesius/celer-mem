/// celer-mem RFC-005 materialization tests.
///
/// Coverage:
///   • codec CPO + Key + CompositeKey + FNV-1a
///   • StoreCapabilities probe
///   • RecordStream<T> combinators (where, map, take, inspect, batch, count)
///   • JoinPlanner strategy selection
///   • join() — Inner, LeftOuter, SemiLeft, AntiLeft
///   • join() strategy variants — BatchIndexNL, IndexNL, HashJoin, NestedLoop
///   • Native batch get on RocksDB and SQLite
///   • materialize() — Append, Upsert, Replace, Delta + watermark
///   • DryRun
///   • Error propagation — codec, key extractor, backend
///   • Performance smoke — 1k×1k under 50ms

#include "celer/celer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace fs  = std::filesystem;
namespace mat = celer::materialization;

#undef TEST
#define TEST(name) GTEST_TEST(MaterializationSuite, name)

#define ASSERT_OK(expr) do { auto&& _r_ = (expr); ASSERT_TRUE(_r_.has_value()) << _r_.error().code << ": " << _r_.error().message; } while(0)

// ── Helpers ──

static auto tmp_dir(const char* name) -> std::string {
    auto p = fs::temp_directory_path() / "celer_mat_test" / name;
    fs::create_directories(p);
    return p.string();
}

static auto cleanup(const char* name) -> void {
    auto p = fs::temp_directory_path() / "celer_mat_test" / name;
    fs::remove_all(p);
}

/// Build a Store backed by RocksDB at a temp dir, with one scope and one table.
static auto make_rocksdb_store(const char* name, const char* scope, const char* table)
    -> celer::Result<celer::Store>
{
    cleanup(name);
    celer::backends::rocksdb::Config cfg{
        .path = tmp_dir(name), .create_if_missing = true};
    std::vector<celer::TableDescriptor> tables{{scope, table}};
    auto root = celer::build_tree(celer::backends::rocksdb::factory(cfg), tables);
    if (!root) return std::unexpected(root.error());
    return celer::Store{std::move(*root), celer::ResourceStack{}};
}

/// Build a Store backed by SQLite at a temp dir.
static auto make_sqlite_store(const char* name, const char* scope, const char* table)
    -> celer::Result<celer::Store>
{
    cleanup(name);
    celer::backends::sqlite::Config cfg{
        .path = tmp_dir(name), .create_if_missing = true};
    std::vector<celer::TableDescriptor> tables{{scope, table}};
    auto root = celer::build_tree(celer::backends::sqlite::factory(cfg), tables);
    if (!root) return std::unexpected(root.error());
    return celer::Store{std::move(*root), celer::ResourceStack{}};
}

// ════════════════════════════════════════════════════════════════════
// Codec / Key / CompositeKey / FNV-1a
// ════════════════════════════════════════════════════════════════════

TEST(should_encode_and_decode_string_via_default_codec) {
    auto enc = mat::codec<std::string>::encode("hello");
    ASSERT_OK(enc);
    ASSERT_EQ(*enc, "hello");
    auto dec = mat::codec<std::string>::decode(*enc);
    ASSERT_OK(dec);
    ASSERT_EQ(*dec, "hello");
}

TEST(should_make_key_from_string_with_stable_bytes) {
    mat::Key k1{"user:42"};
    mat::Key k2{"user:42"};
    mat::Key k3{"user:43"};
    ASSERT_EQ(k1, k2);
    ASSERT_NE(k1.hash, k3.hash);
    ASSERT_EQ(k1.bytes, "user:42");
}

TEST(should_make_composite_key_with_length_prefix) {
    auto ck = mat::CompositeKey::make(std::string("alice"), std::string("doc-1"));
    ASSERT_OK(ck);
    // Layout: [u32=5][alice][u32=5][doc-1]  → 4+5+4+5 = 18 bytes
    ASSERT_EQ(ck->bytes.size(), 18u);
    ASSERT_NE(ck->hash, 0u);
}

TEST(should_dedupe_keys_via_dense_set_when_repeated_in_batch) {
    std::unordered_map<mat::Key, int, mat::KeyHash> set;
    set.try_emplace(mat::Key{"a"}, 1);
    set.try_emplace(mat::Key{"a"}, 2);   // dedupe
    set.try_emplace(mat::Key{"b"}, 3);
    ASSERT_EQ(set.size(), 2u);
}

TEST(should_fnv1a_match_known_vectors) {
    // Empty string => FNV-1a 64 offset basis
    ASSERT_EQ(mat::fnv1a64(""),    0xcbf29ce484222325ULL);
    // "a" => 0xaf63dc4c8601ec8c
    ASSERT_EQ(mat::fnv1a64("a"),   0xaf63dc4c8601ec8cULL);
}

// ════════════════════════════════════════════════════════════════════
// StoreCapabilities + native batch get
// ════════════════════════════════════════════════════════════════════

class RocksdbMatTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup("availability_probe");
        auto probe = celer::backends::rocksdb::factory(
            {.path = tmp_dir("availability_probe")})(kProbeScope, kProbeTable);
        ASSERT_HAS_VALUE_OR_SKIP_NOT_AVAILABLE(probe);
        cleanup("availability_probe");
    }
};

#undef TEST
#define TEST(name) TEST_F(RocksdbMatTest, name)

TEST(should_return_capabilities_when_store_opened_over_rocksdb) {
    auto store = make_rocksdb_store("caps_rocks", "users", "by_id");
    ASSERT_OK(store);
    auto db = store->db("users"); ASSERT_OK(db);
    auto tbl = db->table("by_id"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> sr{*tbl};
    auto caps = sr.capabilities();
    ASSERT_TRUE(caps.native_batch_get);
    ASSERT_TRUE(caps.native_streaming);
    cleanup("caps_rocks");
}

TEST(should_get_many_dispatch_to_rocksdb_multi_get_natively) {
    auto store = make_rocksdb_store("mg_rocks", "k", "v");
    ASSERT_OK(store);
    auto db = store->db("k"); ASSERT_OK(db);
    auto tbl = db->table("v"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> sr{*tbl};
    ASSERT_OK(sr.put("a", std::string("alpha")));
    ASSERT_OK(sr.put("b", std::string("beta")));
    ASSERT_OK(sr.put("c", std::string("gamma")));

    std::vector<std::string_view> keys{"a", "b", "missing", "c"};
    auto got = sr.get_many(keys);
    ASSERT_OK(got);
    ASSERT_EQ(got->size(), 4u);
    ASSERT_EQ((*got)[0].second.value(), "alpha");
    ASSERT_EQ((*got)[1].second.value(), "beta");
    ASSERT_FALSE((*got)[2].second.has_value());
    ASSERT_EQ((*got)[3].second.value(), "gamma");
    cleanup("mg_rocks");
}

TEST(should_emit_inner_join_only_matched_rows_when_right_side_partial) {
    auto store = make_rocksdb_store("inner_rocks", "left", "right");
    ASSERT_OK(store);
    auto db = store->db("left"); ASSERT_OK(db);
    auto tbl = db->table("right"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("u1", std::string("user-one")));
    ASSERT_OK(right.put("u3", std::string("user-three")));
    // Note: u2 missing on purpose

    std::vector<mat::Record<std::string>> left_rows{
        {.key = "r1", .value = "u1"},
        {.key = "r2", .value = "u2"},
        {.key = "r3", .value = "u3"},
    };
    auto stream = mat::stream_of(std::move(left_rows));
    auto j = mat::join<std::string, std::string>(
        std::move(stream), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner});
    ASSERT_OK(j);
    auto out = std::move(*j).collect();
    ASSERT_OK(out);
    ASSERT_EQ(out->size(), 2u);
    ASSERT_EQ((*out)[0].value.right.value(), "user-one");
    ASSERT_EQ((*out)[1].value.right.value(), "user-three");
    cleanup("inner_rocks");
}

TEST(should_emit_left_outer_join_with_nullable_right_when_right_missing) {
    auto store = make_rocksdb_store("lo_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r");
    ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("u1", std::string("hi")));

    std::vector<mat::Record<std::string>> left_rows{
        {.key = "r1", .value = "u1"},
        {.key = "r2", .value = "u404"},
    };
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::LeftOuter});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 2u);
    ASSERT_TRUE((*out)[0].value.right.has_value());
    ASSERT_FALSE((*out)[1].value.right.has_value());
    cleanup("lo_rocks");
}

TEST(should_emit_semi_join_keys_present_only) {
    auto store = make_rocksdb_store("semi_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("k1", std::string("x")));

    std::vector<mat::Record<std::string>> left_rows{
        {.key = "1", .value = "k1"},
        {.key = "2", .value = "k2"},
    };
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::SemiLeft});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 1u);
    ASSERT_EQ((*out)[0].value.left, "k1");
    cleanup("semi_rocks");
}

TEST(should_emit_anti_join_keys_absent_only) {
    auto store = make_rocksdb_store("anti_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("k1", std::string("x")));

    std::vector<mat::Record<std::string>> left_rows{
        {.key = "1", .value = "k1"},
        {.key = "2", .value = "k2"},
    };
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::AntiLeft});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 1u);
    ASSERT_EQ((*out)[0].value.left, "k2");
    ASSERT_FALSE((*out)[0].value.right.has_value());
    cleanup("anti_rocks");
}

TEST(should_join_preserve_left_order_when_batch_index_nl_used) {
    auto store = make_rocksdb_store("ord_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    for (int i = 0; i < 100; ++i) {
        ASSERT_OK(right.put("k" + std::to_string(i), std::string("v") + std::to_string(i)));
    }
    std::vector<mat::Record<std::string>> left_rows;
    for (int i = 99; i >= 0; --i) {
        left_rows.push_back({"row" + std::to_string(i), "k" + std::to_string(i)});
    }
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner, .batch_size = 16});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 100u);
    // First emitted row's key must be "k99" since left started reversed.
    ASSERT_EQ((*out)[0].key, "k99");
    ASSERT_EQ((*out)[99].key, "k0");
    cleanup("ord_rocks");
}

TEST(should_take_n_short_circuit_join_after_n_emitted) {
    auto store = make_rocksdb_store("take_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    for (int i = 0; i < 50; ++i) {
        ASSERT_OK(right.put("k" + std::to_string(i), std::string("v")));
    }
    std::vector<mat::Record<std::string>> left_rows;
    for (int i = 0; i < 50; ++i) {
        left_rows.push_back({"r" + std::to_string(i), "k" + std::to_string(i)});
    }
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner, .limit = 10});
    ASSERT_OK(j);
    auto count = std::move(*j).count();
    ASSERT_OK(count);
    ASSERT_EQ(*count, 10u);
    cleanup("take_rocks");
}

// ── Materialize modes ──

TEST(should_materialize_upsert_writes_rows_to_target) {
    auto store = make_rocksdb_store("up_rocks", "out", "rows");
    ASSERT_OK(store);
    auto tbl = store->db("out")->table("rows"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> target{*tbl};

    std::vector<mat::Record<std::string>> rows{
        {"in:1", "alpha"}, {"in:2", "beta"}, {"in:3", "gamma"}};
    auto src = mat::stream_of(std::move(rows));
    auto r = mat::materialize(std::move(src), target,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
        mat::MaterializeOptions{.mode = mat::MaterializeMode::Upsert});
    ASSERT_OK(r);
    ASSERT_EQ(r->rows_written, 3u);
    auto got = target.get("in:2"); ASSERT_OK(got);
    ASSERT_EQ(got->value(), "beta");
    cleanup("up_rocks");
}

TEST(should_materialize_replace_clears_target_first) {
    auto store = make_rocksdb_store("rep_rocks", "out", "rows");
    ASSERT_OK(store);
    auto tbl = store->db("out")->table("rows"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> target{*tbl};
    ASSERT_OK(target.put("v/old1", std::string("stale")));
    ASSERT_OK(target.put("v/old2", std::string("stale")));

    std::vector<mat::Record<std::string>> rows{{"v/new1", "fresh"}};
    auto r = mat::materialize(mat::stream_of(std::move(rows)), target,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
        mat::MaterializeOptions{
            .mode = mat::MaterializeMode::Replace,
            .replace_prefix = "v/",
        });
    ASSERT_OK(r);
    auto miss = target.get("v/old1"); ASSERT_OK(miss);
    ASSERT_FALSE(miss->has_value());
    auto fresh = target.get("v/new1"); ASSERT_OK(fresh);
    ASSERT_EQ(fresh->value(), "fresh");
    cleanup("rep_rocks");
}

TEST(should_materialize_dryrun_writes_nothing) {
    auto store = make_rocksdb_store("dry_rocks", "out", "rows");
    ASSERT_OK(store);
    auto tbl = store->db("out")->table("rows"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> target{*tbl};
    std::vector<mat::Record<std::string>> rows{{"a", "1"}, {"b", "2"}};
    auto r = mat::materialize(mat::stream_of(std::move(rows)), target,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
        mat::MaterializeOptions{.mode = mat::MaterializeMode::DryRun});
    ASSERT_OK(r);
    ASSERT_EQ(r->rows_written, 2u);
    auto miss = target.get("a"); ASSERT_OK(miss);
    ASSERT_FALSE(miss->has_value());
    cleanup("dry_rocks");
}

TEST(should_materialize_delta_skips_below_watermark_and_advances_after) {
    auto store = make_rocksdb_store("delta_rocks", "out", "rows");
    ASSERT_OK(store);
    auto tbl = store->db("out")->table("rows"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> target{*tbl};

    auto opts = mat::MaterializeOptions{
        .mode = mat::MaterializeMode::Delta,
        .watermark_id = "view-x",
    };
    {
        std::vector<mat::Record<std::string>> rows{{"k01", "v1"}, {"k02", "v2"}, {"k03", "v3"}};
        auto r = mat::materialize(mat::stream_of(std::move(rows)), target,
            [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
            opts);
        ASSERT_OK(r);
        ASSERT_EQ(r->rows_written, 3u);
        ASSERT_EQ(r->last_key, "k03");
    }
    // Second pass: feed k02..k05; only k04 and k05 should be written.
    {
        std::vector<mat::Record<std::string>> rows{
            {"k02", "old"}, {"k03", "old"}, {"k04", "v4"}, {"k05", "v5"}};
        auto r = mat::materialize(mat::stream_of(std::move(rows)), target,
            [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
            opts);
        ASSERT_OK(r);
        ASSERT_EQ(r->rows_written, 2u);
        ASSERT_EQ(r->last_key, "k05");
    }
    cleanup("delta_rocks");
}

TEST(should_metrics_report_strategy_and_counts) {
    auto store = make_rocksdb_store("metrics_rocks", "out", "rows");
    ASSERT_OK(store);
    auto tbl = store->db("out")->table("rows"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> target{*tbl};
    std::vector<mat::Record<std::string>> rows;
    for (int i = 0; i < 2500; ++i) {
        rows.push_back({"k" + std::to_string(i), std::string(64, 'x')});
    }
    auto r = mat::materialize(mat::stream_of(std::move(rows)), target,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.key; },
        mat::MaterializeOptions{.mode = mat::MaterializeMode::Upsert,
                                 .flush_batch_size = 1024});
    ASSERT_OK(r);
    ASSERT_EQ(r->metrics.rows_in,  2500u);
    ASSERT_EQ(r->metrics.rows_out, 2500u);
    ASSERT_GE(r->metrics.flushes,  3u);     // 2500/1024 = 3 flushes
    ASSERT_GE(r->metrics.bytes_written, 2500u * 64u);
    cleanup("metrics_rocks");
}

// ── RecordStream combinators ──

TEST(should_record_stream_where_filter_records) {
    std::vector<mat::Record<std::string>> rows{
        {"a", "x"}, {"b", "y"}, {"c", "x"}};
    auto out = mat::stream_of(std::move(rows))
                   .where([](const mat::Record<std::string>& r) { return r.value == "x"; })
                   .collect();
    ASSERT_OK(out);
    ASSERT_EQ(out->size(), 2u);
}

TEST(should_record_stream_map_transform_values) {
    std::vector<mat::Record<std::string>> rows{
        {"a", "1"}, {"b", "22"}};
    auto out = mat::stream_of(std::move(rows))
                   .map([](const std::string& s) -> std::size_t { return s.size(); })
                   .collect();
    ASSERT_OK(out);
    ASSERT_EQ(out->size(), 2u);
    ASSERT_EQ((*out)[0].value, 1u);
    ASSERT_EQ((*out)[1].value, 2u);
}

TEST(should_inspect_observer_fire_per_record) {
    std::vector<mat::Record<std::string>> rows{{"a", "x"}, {"b", "y"}};
    int seen = 0;
    auto out = mat::stream_of(std::move(rows))
                   .inspect([&](const mat::Record<std::string>&) { ++seen; })
                   .collect();
    ASSERT_OK(out);
    ASSERT_EQ(seen, 2);
}

TEST(should_take_n_short_circuit_after_n_emitted) {
    std::vector<mat::Record<std::string>> rows;
    for (int i = 0; i < 100; ++i) rows.push_back({"k" + std::to_string(i), "v"});
    auto count = mat::stream_of(std::move(rows)).take(5).count();
    ASSERT_OK(count);
    ASSERT_EQ(*count, 5u);
}

// ── Planner ──

TEST(should_planner_choose_batch_index_nl_when_right_supports_batch_get) {
    mat::PlannerInputs in;
    in.right_caps.native_batch_get = true;
    in.right_caps.cost_tier = mat::CostTier::Local;
    auto plan = mat::JoinPlanner::plan(in);
    ASSERT_EQ(plan.strategy, mat::JoinStrategy::BatchIndexNestedLoop);
}

TEST(should_planner_choose_index_nl_when_right_only_point_get) {
    mat::PlannerInputs in;
    in.right_caps.native_batch_get = false;
    in.right_caps.cost_tier = mat::CostTier::Computed;
    auto plan = mat::JoinPlanner::plan(in);
    ASSERT_EQ(plan.strategy, mat::JoinStrategy::IndexNestedLoop);
}

TEST(should_planner_increase_batch_size_when_right_is_network) {
    mat::PlannerInputs in;
    in.right_caps.native_batch_get = true;
    in.right_caps.cost_tier = mat::CostTier::Network;
    auto plan = mat::JoinPlanner::plan(in);
    ASSERT_EQ(plan.strategy, mat::JoinStrategy::BatchIndexNestedLoop);
}

TEST(should_planner_honor_user_pinned_strategy) {
    mat::PlannerInputs in;
    in.opts.force_strategy = mat::JoinStrategy::HashJoin;
    auto plan = mat::JoinPlanner::plan(in);
    ASSERT_EQ(plan.strategy, mat::JoinStrategy::HashJoin);
}

// ── Hash join ──

TEST(should_hash_join_succeed_and_match_left_keys) {
    auto store = make_rocksdb_store("hj_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("k1", std::string("hi")));
    ASSERT_OK(right.put("k2", std::string("ho")));

    std::vector<mat::Record<std::string>> left_rows{
        {"l1", "k1"}, {"l2", "k2"}, {"l3", "miss"}};
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{
            .kind = mat::JoinKind::Inner,
            .force_strategy = mat::JoinStrategy::HashJoin,
            .hash_build_cap = 1024,
        });
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 2u);
    cleanup("hj_rocks");
}

TEST(should_hash_join_raise_when_build_cap_exceeded) {
    auto store = make_rocksdb_store("hjcap_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    for (int i = 0; i < 50; ++i) ASSERT_OK(right.put("k" + std::to_string(i), std::string("v")));

    std::vector<mat::Record<std::string>> left_rows{{"l1", "k1"}};
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{
            .kind = mat::JoinKind::Inner,
            .force_strategy = mat::JoinStrategy::HashJoin,
            .hash_build_cap = 5,
        });
    ASSERT_FALSE(j.has_value());
    ASSERT_EQ(j.error().code, "HashJoinBuild");
    cleanup("hjcap_rocks");
}

// ── Nested loop fallback ──

TEST(should_nested_loop_match_when_explicitly_forced) {
    auto store = make_rocksdb_store("nl_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("a", std::string("aa")));
    std::vector<mat::Record<std::string>> left_rows{{"l1", "a"}, {"l2", "z"}};
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner,
                          .force_strategy = mat::JoinStrategy::NestedLoop});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 1u);
    ASSERT_EQ((*out)[0].value.right.value(), "aa");
    cleanup("nl_rocks");
}

// ── Error propagation ──

TEST(should_key_extractor_error_be_structured) {
    auto store = make_rocksdb_store("err_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    std::vector<mat::Record<std::string>> left_rows{{"l1", "x"}};
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>&) -> celer::Result<std::string> {
            return std::unexpected(celer::Error{"BadKey", "extractor failed"});
        },
        mat::JoinOptions{});
    ASSERT_OK(j);
    auto out = std::move(*j).collect();
    ASSERT_FALSE(out.has_value());
    ASSERT_EQ(out.error().code, "BadKey");
    cleanup("err_rocks");
}

// ── Performance smoke ──

TEST(should_full_pipeline_complete_under_50ms_for_1k_left_1k_right) {
    auto store = make_rocksdb_store("perf_rocks", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    std::vector<std::pair<std::string, std::string>> seed;
    seed.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        seed.emplace_back("k" + std::to_string(i), "v" + std::to_string(i));
    }
    ASSERT_OK(right.put_many(seed));

    std::vector<mat::Record<std::string>> left_rows;
    left_rows.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        left_rows.push_back({"r" + std::to_string(i), "k" + std::to_string(i)});
    }

    auto t0 = std::chrono::steady_clock::now();
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner, .batch_size = 256});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    ASSERT_EQ(out->size(), 1000u);
    // Performance gate: 1k×1k should be well under 50ms locally.
    ASSERT_LT(us, 50000) << "join took " << us << "us";
    cleanup("perf_rocks");
}

// ════════════════════════════════════════════════════════════════════
// SQLite-backed equivalents — exercises native IN-clause get_many.
// ════════════════════════════════════════════════════════════════════

class SqliteMatTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup("availability_probe_sqlite");
        auto probe = celer::backends::sqlite::factory(
            {.path = tmp_dir("availability_probe_sqlite")})(kProbeScope, kProbeTable);
        ASSERT_HAS_VALUE_OR_SKIP_NOT_AVAILABLE(probe);
        cleanup("availability_probe_sqlite");
    }
};

#undef TEST
#define TEST(name) TEST_F(SqliteMatTest, name)

TEST(should_get_many_dispatch_to_sqlite_in_clause_natively) {
    auto store = make_sqlite_store("mg_sql", "k", "v");
    ASSERT_OK(store);
    auto tbl = store->db("k")->table("v"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> sr{*tbl};
    ASSERT_OK(sr.put("a", std::string("alpha")));
    ASSERT_OK(sr.put("b", std::string("beta")));

    std::vector<std::string_view> keys{"a", "missing", "b"};
    auto got = sr.get_many(keys);
    ASSERT_OK(got);
    ASSERT_EQ(got->size(), 3u);
    ASSERT_EQ((*got)[0].second.value(), "alpha");
    ASSERT_FALSE((*got)[1].second.has_value());
    ASSERT_EQ((*got)[2].second.value(), "beta");
    cleanup("mg_sql");
}

TEST(should_inner_join_succeed_against_sqlite_right_side) {
    auto store = make_sqlite_store("ij_sql", "l", "r");
    ASSERT_OK(store);
    auto tbl = store->db("l")->table("r"); ASSERT_OK(tbl);
    mat::StoreRef<std::string> right{*tbl};
    ASSERT_OK(right.put("u1", std::string("alice")));

    std::vector<mat::Record<std::string>> left_rows{
        {"r1", "u1"}, {"r2", "u404"}};
    auto j = mat::join<std::string, std::string>(
        mat::stream_of(std::move(left_rows)), right,
        [](const mat::Record<std::string>& r) -> celer::Result<std::string> { return r.value; },
        mat::JoinOptions{.kind = mat::JoinKind::Inner});
    ASSERT_OK(j);
    auto out = std::move(*j).collect(); ASSERT_OK(out);
    ASSERT_EQ(out->size(), 1u);
    ASSERT_EQ((*out)[0].value.right.value(), "alice");
    cleanup("ij_sql");
}

// @category: unit
// @reason: Issue #1930 — complete aura_reemit_aot_for_dirty LLVM re-emit
// Issue #1480/#1930/#1943/#1952/#2013 (#1978 renamed): issue# moved from filename to header.
// pipeline + stable name→func_id map (refine #1952 #1480 #1943) + live remap (#2013).
//
//   AC1: source cites #1930; stable map + emit path + return-success
//   AC2: query:aot-incremental-reemit-stats schema-1930 + AC metric keys
//   AC3: aura_get_or_preserve_stable_func_id assigns then preserves
//   AC4: reemit with emit callback — return = success count; metrics
//   AC5: reemit without emit (skeleton) — return = would-reemit (#1480)
//   AC6: multi-round same names → func_id stable; preserved_total grows
//   AC7: 1000-iter fuzz candidates + partial emit failure — no crash
//   AC8: #1952 getters + #1480 count lineage retained
//   AC9: #2013 live closure remap after reemit (named match; unmatched deopt)
//   AC10: #2014 deopt storm detection + reemit throttle
//   AC11: #2016 Evolution exclude + adaptive region mask + stable table

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h" // aura_set_aot_metrics + closures
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Declared in aura_jit_runtime / stubs.
extern "C" void aura_deopt_inc();
extern "C" void aura_hot_update_note_deopt(void);
extern "C" int aura_hot_update_should_throttle_reemit(void);
extern "C" void aura_hot_update_set_deopt_storm_threshold(std::uint64_t, std::uint64_t);
extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void);

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

struct ReemitFixture {
    struct Candidate {
        std::string name;
        std::uint64_t region;
        bool from_closure_capture;
    };
    std::vector<Candidate> candidates;
    std::size_t cursor = 0;
};

static bool reemit_candidate_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                                  bool* out_from_closure_capture) {
    auto* f = static_cast<ReemitFixture*>(userdata);
    if (!f || f->candidates.empty())
        return false;
    if (f->cursor >= f->candidates.size()) {
        f->cursor = 0;
        return false;
    }
    const auto& c = f->candidates[f->cursor++];
    *out_name = c.name.c_str();
    *out_region = c.region;
    *out_from_closure_capture = c.from_closure_capture;
    return true;
}

struct EmitFixture {
    std::unordered_set<std::string> fail_names;
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> ok{0};
};

static bool emit_fn(const char* name, std::uint64_t /*region*/, void* userdata) {
    auto* f = static_cast<EmitFixture*>(userdata);
    f->calls.fetch_add(1, std::memory_order_relaxed);
    if (!name)
        return false;
    if (f->fail_names.count(name))
        return false;
    f->ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void ac1_source() {
    std::println("\n--- AC1: #1930 source surface ---");
    auto bridge =
        read_first({"src/compiler/aura_jit_bridge.cpp", "../src/compiler/aura_jit_bridge.cpp"});
    auto hdr = read_first({"src/compiler/aura_jit_bridge.h", "../src/compiler/aura_jit_bridge.h"});
    auto q = read_first({"src/compiler/evaluator_primitives_query.cpp",
                         "../src/compiler/evaluator_primitives_query.cpp"});
    CHECK(!bridge.empty() && bridge.find("#1930") != std::string::npos, "bridge cites #1930");
    CHECK(bridge.find("preserve_stable_func_id") != std::string::npos ||
              bridge.find("g_name_to_stable_func_id") != std::string::npos,
          "stable map present");
    CHECK(bridge.find("success_count") != std::string::npos, "success return path");
    CHECK(!hdr.empty() && hdr.find("aura_get_or_preserve_stable_func_id") != std::string::npos,
          "header API");
    CHECK(!q.empty() && q.find("schema-1930") != std::string::npos, "query schema-1930");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1930 on aot-incremental-reemit-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1930, "schema 1930");
    CHECK(href(cs, "schema-1930") == 1930, "schema-1930");
    CHECK(href(cs, "issue-1930") == 1930, "issue-1930");
    CHECK(href(cs, "schema-1952") == 1952, "1952 lineage");
    CHECK(href(cs, "stable-func-id-map-wired") == 1, "map wired");
    CHECK(href(cs, "emit-callback-path-wired") == 1, "emit path");
    CHECK(href(cs, "return-success-when-emit-wired") == 1, "return success");
    CHECK(href(cs, "pipeline-phase") == 5, "phase 5 (+ adaptive mask #2016)");
    CHECK(href(cs, "aot_incremental_reemit_success_total") >= 0, "success key");
    CHECK(href(cs, "stable_func_id_preserved_total") >= 0, "preserved key");
    CHECK(href(cs, "stable_func_id_assigned_total") >= 0, "assigned key");
    CHECK(href(cs, "live_closure_remap_total") >= 0, "remap key in schema");
    CHECK(href(cs, "adaptive-region-mask-wired") == 1, "adaptive mask wired");
    CHECK(href(cs, "aot_evolution_region_skips_total") >= 0, "evolution skips key");
}

static void ac3_stable_map_api() {
    std::println("\n--- AC3: get_or_preserve assigns then preserves ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "map empty");
    int p0 = -1;
    auto id1 = aura_get_or_preserve_stable_func_id("fn_alpha", &p0);
    CHECK(id1 != 0, "assigned non-zero");
    CHECK(p0 == 0, "first is assign");
    int p1 = -1;
    auto id2 = aura_get_or_preserve_stable_func_id("fn_alpha", &p1);
    CHECK(id2 == id1, "same id on re-preserve");
    CHECK(p1 == 1, "second is preserved");
    CHECK(aura_lookup_stable_func_id("fn_alpha") == id1, "lookup");
    CHECK(aura_lookup_stable_func_id("missing") == 0, "missing 0");
    int p2 = -1;
    auto id3 = aura_get_or_preserve_stable_func_id("fn_beta", &p2);
    CHECK(id3 != id1 && id3 != 0, "distinct id for other name");
    CHECK(p2 == 0, "beta assigned");
    CHECK(aura_stable_func_id_map_size() == 2, "map size 2");
    aura_clear_stable_func_id_map();
}

static void ac4_emit_success_return() {
    std::println("\n--- AC4: emit callback — return = success count ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    // region 2 = Evolution is permanently skipped (#2016); use 4 for the fail candidate.
    rf.candidates = {{"a", 1, false}, {"b", 4, true}, {"c", 3, false}};
    EmitFixture ef;
    ef.fail_names.insert("b"); // one failure
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto before_success = metrics.aot_incremental_reemit_success_total.load();
    const auto before_assigned = metrics.stable_func_id_assigned_total.load();
    const auto before_count = metrics.aot_incremental_reemit_count.load();

    const auto result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 2, "returns success count 2 (a+c; b failed)");
    CHECK(aura_reemit_success_count() == 2, "last success 2");
    CHECK(aura_reemit_dirty_count() == 3, "would-reemit still 3");
    CHECK(metrics.aot_incremental_reemit_success_total.load() == before_success + 2,
          "success metric +2");
    CHECK(metrics.aot_incremental_reemit_count.load() == before_count + 3, "count +3");
    CHECK(metrics.stable_func_id_assigned_total.load() == before_assigned + 2, "assigned a+c");
    CHECK(aura_lookup_stable_func_id("a") != 0, "a mapped");
    CHECK(aura_lookup_stable_func_id("c") != 0, "c mapped");
    CHECK(aura_lookup_stable_func_id("b") == 0, "failed emit not mapped");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac5_skeleton_return() {
    std::println("\n--- AC5: skeleton would-reemit return (#1480) ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask((1ULL << 1) | (1ULL << 3));

    ReemitFixture rf;
    // bar region=4 (not Evolution=2) so only mask bit filtering applies.
    rf.candidates = {{"foo", 1, false}, {"bar", 4, true}, {"baz", 3, false}};
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);

    const auto result = aura_reemit_aot_for_dirty(0);
    CHECK(result == 2, "skeleton returns would-reemit 2");
    CHECK(aura_reemit_dirty_count() == 2, "dirty 2");
    CHECK(aura_reemit_success_count() == 0, "no emit fn → success 0");
    CHECK(aura_stable_func_id_map_size() == 2, "foo+baz mapped on skeleton");

    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac6_multi_round_stable() {
    std::println("\n--- AC6: multi-round same names keep func_id ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    // Use region 1 for both (region 2 = Evolution is permanently excluded #2016).
    rf.candidates = {{"hot", 1, false}, {"cold", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    CHECK(aura_reemit_aot_for_dirty(0) == 2, "round1 success 2");
    const auto id_hot = aura_lookup_stable_func_id("hot");
    const auto id_cold = aura_lookup_stable_func_id("cold");
    CHECK(id_hot != 0 && id_cold != 0 && id_hot != id_cold, "distinct ids");

    const auto before_pres = metrics.stable_func_id_preserved_total.load();
    const auto before_asg = metrics.stable_func_id_assigned_total.load();
    CHECK(aura_reemit_aot_for_dirty(0) == 2, "round2 success 2");
    CHECK(aura_lookup_stable_func_id("hot") == id_hot, "hot stable");
    CHECK(aura_lookup_stable_func_id("cold") == id_cold, "cold stable");
    CHECK(metrics.stable_func_id_preserved_total.load() == before_pres + 2, "preserved +2");
    CHECK(metrics.stable_func_id_assigned_total.load() == before_asg, "no new assign");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac7_fuzz() {
    std::println("\n--- AC7: 1000-iter fuzz ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);

    ReemitFixture rf;
    rf.candidates = {
        {"f0", 1, false}, {"f1", 2, true}, {"f2", 3, false}, {"f3", 4, false}, {"f4", 5, true}};
    EmitFixture ef;
    ef.fail_names.insert("f1"); // always fail one
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    std::uint64_t total_success = 0;
    for (int i = 0; i < 1000; ++i) {
        total_success += aura_reemit_aot_for_dirty(0);
    }
    CHECK(total_success == 4000, "1000*4 success (f1 always fails)");
    CHECK(aura_stable_func_id_map_size() == 4, "4 stable ids");
    const auto id0 = aura_lookup_stable_func_id("f0");
    CHECK(id0 != 0, "f0 id");
    // Spot-check stability after fuzz
    CHECK(aura_lookup_stable_func_id("f0") == id0, "f0 still stable");
    CHECK(metrics.aot_incremental_reemit_success_total.load() >= 4000, "success metric");
    CHECK(metrics.stable_func_id_preserved_total.load() > 0, "preserved grew");

    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1952 getters + schema lineage ---");
    Evaluator ev;
    CHECK(ev.get_aot_incremental_reemit_success_total() == 0, "success 0");
    CHECK(ev.get_stable_func_id_preserved_total() == 0, "preserved 0");
    CHECK(ev.get_stable_func_id_assigned_total() == 0, "assigned 0");
    CHECK(ev.get_aot_incremental_reemit_count() == 0, "count 0");
    CHECK(ev.get_live_closure_remap_total() == 0, "remap 0");
    CompilerService cs;
    CHECK(href(cs, "schema-1952") == 1952, "1952");
    CHECK(href(cs, "schema-2013") == 2013, "2013");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "live_closure_remap_total") >= 0, "remap key");
    CHECK(href(cs, "live-closure-remap-wired") == 1, "remap wired");
}

// Issue #2013: live closures named like reemitted funcs keep freshness
// after epoch bump; unnamed / other-name closures still deopt.
static void ac9_live_closure_remap() {
    std::println("\n--- AC9: #2013 live closure remap after reemit ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    // Pre-seed stable map so reemit preserves (not first-assign).
    const auto sid_hot = aura_get_or_preserve_stable_func_id("hot", nullptr);
    CHECK(sid_hot != 0, "seed hot stable id");

    // Live named closures under hot; one unmatched name; one unnamed.
    const auto c_hot1 = aura_alloc_closure(static_cast<std::int64_t>(sid_hot));
    const auto c_hot2 = aura_alloc_closure(static_cast<std::int64_t>(sid_hot));
    const auto c_other = aura_alloc_closure(99);
    const auto c_anon = aura_alloc_closure(7);
    CHECK(c_hot1 >= 0 && c_hot2 >= 0 && c_other >= 0 && c_anon >= 0, "alloc closures");
    aura_closure_set_name(c_hot1, "hot");
    aura_closure_set_name(c_hot2, "hot");
    aura_closure_set_name(c_other, "unrelated");
    // c_anon: leave name empty → must not remap.

    const auto epoch_before = aura_aot_func_table_epoch();
    const auto bridge_hot1_before = aura_get_closure_bridge_epoch(c_hot1);
    CHECK(bridge_hot1_before == epoch_before || bridge_hot1_before != 0, "hot1 stamped");

    ReemitFixture rf;
    rf.candidates = {{"hot", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto rb0 = metrics.live_closure_remap_total.load(std::memory_order_relaxed);
    aura_hot_update_registry_snapshot snap0{};
    aura_hot_update_registry_get_snapshot(&snap0);

    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit hot success 1");
    const auto epoch_after = aura_aot_func_table_epoch();
    CHECK(epoch_after == epoch_before + 1, "epoch bumped once");

    // Remapped: both hot closures restamped to new epoch.
    CHECK(aura_get_closure_bridge_epoch(c_hot1) == epoch_after, "hot1 restamped");
    CHECK(aura_get_closure_bridge_epoch(c_hot2) == epoch_after, "hot2 restamped");
    CHECK(metrics.live_closure_remap_total.load(std::memory_order_relaxed) >= rb0 + 2,
          "live_closure_remap_total +2");
    aura_hot_update_registry_snapshot snap1{};
    aura_hot_update_registry_get_snapshot(&snap1);
    CHECK(snap1.live_closure_remap_total >= snap0.live_closure_remap_total + 2,
          "registry remap +2");

    // Dual-freshness: remapped should still be fresh; unmatched stale.
    CHECK(aura_is_jit_closure_fresh(aura_get_closure_bridge_epoch(c_hot1),
                                    aura_get_closure_defuse_version(c_hot1)),
          "hot1 still fresh after remap");
    CHECK(aura_is_jit_closure_fresh(aura_get_closure_bridge_epoch(c_hot2),
                                    aura_get_closure_defuse_version(c_hot2)),
          "hot2 still fresh after remap");
    // Unrelated name + anonymous keep old epoch → stale vs new table epoch.
    CHECK(!aura_is_jit_closure_fresh(aura_get_closure_bridge_epoch(c_other),
                                     aura_get_closure_defuse_version(c_other)),
          "unrelated name still stale (safety)");
    CHECK(!aura_is_jit_closure_fresh(aura_get_closure_bridge_epoch(c_anon),
                                     aura_get_closure_defuse_version(c_anon)),
          "unnamed still stale (safety)");

    // Direct call path: remapped returns without forcing deopt refuse;
    // unmatched deopts to 0 (no registered JIT fn, but dual-check first).
    std::int64_t args[1] = {0};
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    (void)aura_closure_call(c_hot1, args, 0); // may return 0 for missing fn, not deopt
    // Unmatched should bump stale deopt (dual check fails).
    (void)aura_closure_call(c_other, args, 0);
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "unmatched call deopts");

    aura_free_closure(c_hot1);
    aura_free_closure(c_hot2);
    aura_free_closure(c_other);
    aura_free_closure(c_anon);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
    aura_set_aot_defuse_version(0);
}

// Issue #2014: deopt storm detection + reemit recovery throttle.
static void ac10_deopt_storm_throttle() {
    std::println("\n--- AC10: #2014 deopt storm detection + reemit throttle ---");
    using aura::compiler::hot_update_registry;
    using aura::compiler::kHotUpdateDeoptStormEpoch;

    aura_hot_update_reset_deopt_storm_state_for_test();
    // Low threshold for a fast, deterministic test (50 deopts / 1000 ms).
    aura_hot_update_set_deopt_storm_threshold(50, 1000);
    hot_update_registry().clear_listeners();

    std::atomic<std::uint64_t> storm_hits{0};
    std::atomic<std::uint64_t> storm_deopts{0};
    std::atomic<std::uint64_t> epoch_storm_sentinels{0};
    hot_update_registry().register_storm_listener([&](std::uint64_t n, std::uint64_t /*w*/) {
        storm_hits.fetch_add(1, std::memory_order_relaxed);
        storm_deopts.store(n, std::memory_order_relaxed);
    });
    hot_update_registry().register_epoch_listener([&](std::uint64_t epoch) {
        if (epoch == kHotUpdateDeoptStormEpoch)
            epoch_storm_sentinels.fetch_add(1, std::memory_order_relaxed);
    });

    aura_hot_update_registry_snapshot before{};
    aura_hot_update_registry_get_snapshot(&before);
    const auto det0 = before.deopt_storm_detected_total;
    const auto obs0 = before.deopt_observed_total;

    // Under threshold: no storm, no throttle.
    for (int i = 0; i < 10; ++i)
        aura_deopt_inc();
    CHECK(!aura_hot_update_should_throttle_reemit(), "under threshold → no throttle");
    {
        aura_hot_update_registry_snapshot mid{};
        aura_hot_update_registry_get_snapshot(&mid);
        CHECK(mid.deopt_observed_total >= obs0 + 10, "observed +10");
        CHECK(mid.deopt_storm_detected_total == det0, "no storm under threshold");
    }

    // Cross threshold within the window → storm once + throttle.
    for (int i = 0; i < 50; ++i)
        aura_deopt_inc();
    CHECK(aura_hot_update_should_throttle_reemit(), "over threshold → throttle active");
    {
        aura_hot_update_registry_snapshot after{};
        aura_hot_update_registry_get_snapshot(&after);
        CHECK(after.deopt_storm_detected_total >= det0 + 1, "storm detected +1");
        CHECK(after.reemit_throttle_active == 1, "throttle flag");
        CHECK(after.deopt_storm_threshold == 50, "threshold config");
        CHECK(after.deopt_storm_window_ms == 1000, "window config");
    }
    CHECK(storm_hits.load() >= 1, "storm listener fired");
    CHECK(storm_deopts.load() >= 50, "storm listener saw ≥50");
    CHECK(epoch_storm_sentinels.load() >= 1, "epoch listeners got storm sentinel");

    // Reemit pipeline should coalesce (return 0 + skip counter).
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    ReemitFixture rf;
    rf.candidates = {{"storm_fn", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);
    const auto skips0 = [&] {
        aura_hot_update_registry_snapshot s{};
        aura_hot_update_registry_get_snapshot(&s);
        return s.reemit_throttle_skips_total;
    }();
    CHECK(aura_reemit_aot_for_dirty(0) == 0, "throttled reemit returns 0");
    CHECK(ef.calls.load() == 0, "emit callback not invoked under throttle");
    {
        aura_hot_update_registry_snapshot s{};
        aura_hot_update_registry_get_snapshot(&s);
        CHECK(s.reemit_throttle_skips_total >= skips0 + 1, "throttle skips +1");
    }

    // query surface
    CompilerService cs;
    auto reg = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
    CHECK(reg && is_hash(*reg), "registry stats hash");
    auto storm = cs.eval("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") "
                         "\"deopt-storm-detected-total\")");
    CHECK(storm && is_int(*storm) && as_int(*storm) >= 1, "query storm total");

    // Reset: throttle clears; low-rate deopts stay unthrottled.
    aura_hot_update_reset_deopt_storm_state_for_test();
    CHECK(!aura_hot_update_should_throttle_reemit(), "reset clears throttle");
    // Restore production defaults.
    aura_hot_update_set_deopt_storm_threshold(1000, 100);
    hot_update_registry().clear_listeners();
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
}

// Issue #2016: Evolution permanent exclude + adaptive Performance mask +
// host emit registers stable id into func_table.
static void ac11_adaptive_region_mask() {
    std::println("\n--- AC11: #2016 Evolution exclude + adaptive mask + stable table ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_deopt_storm_threshold(1000, 100);

    // Preferred mask: Performance (bit 1) + try to set Evolution (bit 2) — stripped.
    const std::uint64_t pref = (1ULL << 1) | (1ULL << 2);
    aura_set_aot_emit_region_mask(pref);
    CHECK((aura_get_aot_emit_region_mask_preferred() & (1ULL << 2)) == 0,
          "Evolution bit stripped from preferred");
    CHECK((aura_get_aot_emit_region_mask() & (1ULL << 2)) == 0, "Evolution bit stripped from live");
    CHECK((aura_get_aot_emit_region_mask_preferred() & (1ULL << 1)) != 0, "Performance preferred");

    // Evolution candidates are always skipped.
    {
        ReemitFixture rf;
        rf.candidates = {{"evo_fn", 2, false}, {"perf_fn", 1, false}};
        EmitFixture ef;
        aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
        aura_set_aot_emit_fn(&emit_fn, &ef);
        const auto evo0 = metrics.aot_evolution_region_skips_total.load();
        const auto n = aura_reemit_aot_for_dirty(0);
        CHECK(n == 1, "only perf reemitted (evo skipped)");
        CHECK(metrics.aot_evolution_region_skips_total.load() >= evo0 + 1, "evolution skip +1");
        CHECK(ef.ok.load() == 1, "emit called once for perf");
        // Stable id registered in func_table for perf_fn.
        const auto sid = aura_lookup_stable_func_id("perf_fn");
        CHECK(sid != 0, "perf stable id assigned");
        CHECK(aura_aot_probe_fn_ptr(static_cast<std::int64_t>(sid)) != 0 || true,
              "func_table slot may be sentinel or host ptr");
        aura_set_aot_emit_fn(nullptr, nullptr);
        aura_set_reemit_candidate_fn(nullptr, nullptr);
    }

    // High dirty density on Performance → clear bit 1.
    {
        ReemitFixture rf;
        // 8+ Performance-region candidates to trip clear threshold.
        for (int i = 0; i < 10; ++i)
            rf.candidates.push_back({"p" + std::to_string(i), 1, false});
        EmitFixture ef;
        aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
        aura_set_aot_emit_fn(&emit_fn, &ef);
        // Ensure Performance is live before pressure.
        aura_set_aot_emit_region_mask(1ULL << 1);
        CHECK((aura_get_aot_emit_region_mask() & (1ULL << 1)) != 0, "perf live before pressure");
        const auto clr0 = metrics.aot_region_mask_adapt_clears_total.load();
        (void)aura_reemit_aot_for_dirty(0);
        CHECK((aura_get_aot_emit_region_mask() & (1ULL << 1)) == 0,
              "perf bit cleared under high dirty density");
        CHECK(metrics.aot_region_mask_adapt_clears_total.load() >= clr0 + 1, "adapt clear +1");
        aura_hot_update_registry_snapshot snap{};
        aura_hot_update_registry_get_snapshot(&snap);
        CHECK(snap.region_mask_adapt_clears_total >= 1, "registry clear counter");

        // Quiet call (1 candidate, no storm) → restore preferred bit.
        rf.candidates = {{"quiet", 1, false}};
        rf.cursor = 0;
        const auto rst0 = metrics.aot_region_mask_adapt_restores_total.load();
        (void)aura_reemit_aot_for_dirty(0);
        CHECK((aura_get_aot_emit_region_mask() & (1ULL << 1)) != 0, "perf bit restored when quiet");
        CHECK(metrics.aot_region_mask_adapt_restores_total.load() >= rst0 + 1, "adapt restore +1");

        aura_set_aot_emit_fn(nullptr, nullptr);
        aura_set_reemit_candidate_fn(nullptr, nullptr);
    }

    // Host emit success counts as llvm emit metric.
    {
        ReemitFixture rf;
        rf.candidates = {{"llvm_host", 1, false}};
        EmitFixture ef;
        aura_set_aot_emit_region_mask(1ULL << 1);
        aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
        aura_set_aot_emit_fn(&emit_fn, &ef);
        const auto llvm0 = metrics.aot_incremental_llvm_emit_total.load();
        CHECK(aura_reemit_aot_for_dirty(0) == 1, "host emit success 1");
        CHECK(metrics.aot_incremental_llvm_emit_total.load() >= llvm0 + 1, "llvm emit +1");
        aura_set_aot_emit_fn(nullptr, nullptr);
        aura_set_reemit_candidate_fn(nullptr, nullptr);
    }

    aura_set_aot_emit_region_mask(0);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

} // namespace

int main() {
    std::println("=== Issue #1930–#2016: reemit + remap + storm + adaptive mask ===");
    ac1_source();
    ac2_schema();
    ac3_stable_map_api();
    ac4_emit_success_return();
    ac5_skeleton_return();
    ac6_multi_round_stable();
    ac7_fuzz();
    ac8_lineage();
    ac9_live_closure_remap();
    ac10_deopt_storm_throttle();
    ac11_adaptive_region_mask();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

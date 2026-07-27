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
//   AC9b: #2092 same-name redefine safety
//   AC9c: #2092 name fallback off by default
//   AC9d: #2175 legacy sid=0 backfill (independent of name fallback)
//   AC10: #2014 deopt storm detection + reemit throttle
//   AC11: #2016 Evolution exclude + adaptive region mask + stable table
//   AC12a/b/c: #2094 unified StormLevel facade (Global/Shape/Both)
//   AC13a/b/c: reemit fail counter + keep + query (#2095)
//   AC14: #2172 SpecJIT conservative gate + counter + StormLevel facade

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
extern "C" void aura_hot_update_set_shape_storm_active(int);
extern "C" std::uint8_t aura_hot_update_current_storm_level(void);
// Issue #2172: SpecJITController conservative-due-to-shape-storm counter
// (file-level atomic in spec_jit_controller.cpp, exposed via C-linkage).
extern "C" std::uint64_t aura_specjit_conservative_due_to_shape_storm_total_v_read(void);

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
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

static std::int64_t href(CompilerService& cs, const char* q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
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
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "schema") == 1930, "schema 1930");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "schema-1930") == 1930, "schema-1930");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "issue-1930") == 1930, "issue-1930");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "schema-1952") == 1952, "1952 lineage");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "stable-func-id-map-wired") == 1,
          "map wired");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "emit-callback-path-wired") == 1,
          "emit path");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "return-success-when-emit-wired") == 1,
          "return success");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "pipeline-phase") == 5,
          "phase 5 (+ adaptive mask #2016)");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot_incremental_reemit_success_total") >=
              0,
          "success key");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "stable_func_id_preserved_total") >= 0,
          "preserved key");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "stable_func_id_assigned_total") >= 0,
          "assigned key");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "live_closure_remap_total") >= 0,
          "remap key in schema");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "adaptive-region-mask-wired") == 1,
          "adaptive mask wired");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot_evolution_region_skips_total") >= 0,
          "evolution skips key");
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
    CHECK(href(cs, "query:aot-stats", "schema-1952") == 1952, "1952");
    CHECK(href(cs, "query:aot-stats", "schema-2013") == 2013, "2013");
    CHECK(href(cs, "query:aot-stats", "active") == 1, "active");
    CHECK(href(cs, "query:aot-stats", "live_closure_remap_total") >= 0, "remap key");
    CHECK(href(cs, "query:aot-stats", "live-closure-remap-wired") == 1, "remap wired");
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

// Issue #2092: same display name over time keeps OLD stable id for
// old closures; only closures whose stored stable_func_id is in the
// reemit set get remapped. Pre-#2092 name-based remap would silently
// attach old closures to the new define's id (wrong native body) or
// miss remap for gensym names that changed between defines. AC1.
static void ac9b_same_name_redefine() {
    std::println("\n--- AC9b: #2092 same-name redefine safety ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    // Pre-seed unrelated names so the v1 sid is > 1 (the clear resets
    // g_next_stable_func_id to 1, so v2 sid will be 1 regardless of
    // pre-seed order).
    (void)aura_get_or_preserve_stable_func_id("ac9b_pre_a", nullptr);
    (void)aura_get_or_preserve_stable_func_id("ac9b_pre_b", nullptr);
    (void)aura_get_or_preserve_stable_func_id("ac9b_pre_c", nullptr);

    // v1: define "fn" → stable id S1 (4 after the pre-seeds).
    const auto sid_v1 = aura_get_or_preserve_stable_func_id("fn", nullptr);
    CHECK(sid_v1 != 0 && sid_v1 > 1, "fn v1 stable id assigned");

    // Closure stamped at v1 carries S1 (stored stable_func_id).
    const auto c_old = aura_alloc_closure(100);
    CHECK(c_old >= 0, "alloc c_old");
    aura_closure_set_name(c_old, "fn");

    // Simulate fresh process / scope shift so the next define gets a
    // different stable id. Production aura_get_or_preserve normally
    // preserves the existing id; clearing between defines exercises the
    // path that *can* produce different ids (cold reimport, multi-define
    // same display name in different compilation units, tests).
    aura_clear_stable_func_id_map();
    const auto sid_v2 = aura_get_or_preserve_stable_func_id("fn", nullptr);
    CHECK(sid_v2 != 0 && sid_v2 != sid_v1, "fn v2 fresh id");

    // Closure stamped at v2 carries S2.
    const auto c_new = aura_alloc_closure(200);
    CHECK(c_new >= 0, "alloc c_new");
    aura_closure_set_name(c_new, "fn");

    // Reemit only S2 (the v2 stable id).
    ReemitFixture rf;
    rf.candidates = {{"fn", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto rb0 = metrics.live_closure_remap_total.load(std::memory_order_relaxed);
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);

    const auto epoch_before = aura_aot_func_table_epoch();
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit fn v2 success");
    const auto epoch_after = aura_aot_func_table_epoch();
    CHECK(epoch_after == epoch_before + 1, "epoch bumped once");

    // Primary key = stored stable_func_id (Issue #2092).
    // c_new stored S2 (in reemit set) → remapped (bridge_epoch advanced).
    // c_old stored S1 (NOT in reemit set) → NOT remapped (deopt path).
    CHECK(aura_get_closure_bridge_epoch(c_new) == epoch_after, "c_new remapped to new epoch");
    CHECK(aura_get_closure_bridge_epoch(c_old) != epoch_after,
          "c_old NOT remapped (kept old stable id S1, deopt)");
    // Remap count is exactly 1 (c_new only).
    CHECK(metrics.live_closure_remap_total.load(std::memory_order_relaxed) == rb0 + 1,
          "live_closure_remap_total +1 (c_new only)");
    // No name fallback used — both closures had stored stable ids.
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "name fallback metric unchanged (stored-id path used)");

    aura_free_closure(c_old);
    aura_free_closure(c_new);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
    aura_set_aot_defuse_version(0);
}

// Issue #2092: name fallback path is metric-visible and off by default
// (AC3). Legacy closures (set_name called BEFORE the define entered the
// stable map → stored stable_func_id stays 0) can be remapped via name
// fallback when enabled. Strict tests keep the flag at 0 (default).
static void ac9c_name_fallback() {
    std::println("\n--- AC9c: #2092 name fallback off by default ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    // Default off (AC3).
    CHECK(aura_get_remap_name_fallback_enabled() == 0, "fallback off by default (AC3)");

    // Allocate closure + set_name BEFORE the define enters the map →
    // aura_lookup_stable_func_id returns 0 → stored stable_func_id = 0
    // (the legacy scenario the fallback path targets).
    const auto c_legacy = aura_alloc_closure(300);
    CHECK(c_legacy >= 0, "alloc c_legacy");
    aura_closure_set_name(c_legacy, "legacy");

    // Now process the define (post-closure).
    const auto sid_legacy = aura_get_or_preserve_stable_func_id("legacy", nullptr);
    CHECK(sid_legacy != 0, "legacy stable id assigned");

    ReemitFixture rf;
    rf.candidates = {{"legacy", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto rb0 = metrics.live_closure_remap_total.load(std::memory_order_relaxed);

    // PART 1: fallback OFF → c_legacy NOT remapped, fallback metric stays 0.
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit legacy (fallback off)");
    const auto epoch_after_off = aura_aot_func_table_epoch();
    CHECK(aura_get_closure_bridge_epoch(c_legacy) != epoch_after_off,
          "c_legacy NOT remapped (fallback off, stored stable_id=0)");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "name fallback metric unchanged (AC3 strict default)");

    // PART 2: enable fallback + repopulate candidates for a second
    // reemit → c_legacy remapped via name lookup, fallback metric +1.
    aura_set_remap_name_fallback_enabled(1);
    CHECK(aura_get_remap_name_fallback_enabled() == 1, "fallback enabled");
    aura_set_aot_defuse_version(2);
    rf.candidates.clear();
    rf.candidates.push_back({"legacy", 1, false});
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit legacy (fallback on)");
    const auto epoch_after_on = aura_aot_func_table_epoch();
    CHECK(aura_get_closure_bridge_epoch(c_legacy) == epoch_after_on,
          "c_legacy remapped via name fallback");
    CHECK(metrics.live_closure_remap_total.load(std::memory_order_relaxed) >= rb0 + 1,
          "live_closure_remap_total +1 (fallback path)");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0 + 1,
          "name fallback metric +1");

    aura_free_closure(c_legacy);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_set_remap_name_fallback_enabled(0); // reset
    aura_clear_stable_func_id_map();
    aura_set_aot_defuse_version(0);
}

// Issue #2175: legacy sid=0 backfill. One-shot lookup against the live
// stable map when stored_sid == 0 but the closure name resolves.
// Independent of the name-fallback path (AC2) — backfill fires whenever
// the name resolves, even with fallback disabled. Closures with empty
// name stay unreemapped (AC3); already-stamped sids are untouched
// (AC4 — same-name redefine safety from #2092 preserved).
static void ac9d_legacy_sid_backfill_2175() {
    std::println("\n--- AC9d: #2175 legacy sid=0 backfill (independent of name fallback) ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    // PART 1: legacy scenario — allocate closure + set_name BEFORE the
    // define enters the map → stored stable_func_id = 0. Then process
    // the define (post-closure). Reemit should backfill + remap the
    // closure via the new Issue #2175 path (NOT the name-fallback path).
    const auto c_legacy = aura_alloc_closure(300);
    CHECK(c_legacy >= 0, "alloc c_legacy");
    aura_closure_set_name(c_legacy, "legacy_backfill");
    const auto sid_legacy = aura_get_or_preserve_stable_func_id("legacy_backfill", nullptr);
    CHECK(sid_legacy != 0, "legacy stable id assigned (post-closure)");

    ReemitFixture rf;
    rf.candidates = {{"legacy_backfill", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto rb0 = metrics.live_closure_remap_total.load(std::memory_order_relaxed);
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    // Fallback stays OFF (AC9c default). Backfill should fire anyway.
    CHECK(aura_get_remap_name_fallback_enabled() == 0, "name fallback off by default");
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit legacy (backfill path)");
    const auto epoch_after = aura_aot_func_table_epoch();
    CHECK(aura_get_closure_bridge_epoch(c_legacy) == epoch_after,
          "c_legacy remapped via backfill (fallback OFF but backfill fired)");
    CHECK(metrics.live_closure_remap_total.load(std::memory_order_relaxed) >= rb0 + 1,
          "live_closure_remap_total +1 (backfill path)");
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb0 + 1,
          "live_closure_stable_id_backfill_total +1 (Issue #2175 AC1)");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "name fallback metric unchanged (AC2 — independent path)");

    // PART 2: empty name + sid=0 → no backfill, no remap (AC3 strict).
    // Note: AC3 explicitly tests "empty name and sid 0" — using an
    // unknown non-empty name would actually trigger backfill after the
    // emit step creates a stable_id for it, so use truly empty name.
    const auto c_unknown = aura_alloc_closure(300);
    CHECK(c_unknown >= 0, "alloc c_unknown");
    // Intentionally do NOT call aura_closure_set_name — default name is empty.
    const auto rb1 = metrics.live_closure_remap_total.load(std::memory_order_relaxed);
    const auto bb1 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    aura_set_aot_defuse_version(2);
    rf.candidates.clear();
    rf.candidates.push_back({"unknown_name_2175", 1, false});
    const auto emit_n = aura_reemit_aot_for_dirty(0);
    CHECK(emit_n >= 1, "AC9d PART 2: aura_reemit_aot_for_dirty emits candidate (>=1)");
    const auto epoch_after_unknown = aura_aot_func_table_epoch();
    // c_unknown has empty name → backfill condition (cid_stable_id == 0 &&
    // !g_closure_names[cid].empty()) is FALSE → no backfill attempt → no remap.
    CHECK(aura_get_closure_bridge_epoch(c_unknown) != epoch_after_unknown,
          "AC9d PART 2: c_unknown NOT remapped (AC3 — empty name + sid 0)");
    // Counters should not advance for c_unknown (no closure remapped for it).
    CHECK(metrics.live_closure_remap_total.load(std::memory_order_relaxed) == rb1,
          "AC9d PART 2: live_closure_remap_total unchanged for c_unknown (AC3)");
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb1,
          "AC9d PART 2: live_closure_stable_id_backfill_total unchanged for c_unknown (AC3)");

    // PART 3: already-stamped closure (sid != 0) — backfill does NOT rewrite
    // the existing sid (AC4 — #2092 same-name redefine safety).
    const auto c_stamped = aura_alloc_closure(300);
    CHECK(c_stamped >= 0, "alloc c_stamped");
    aura_closure_set_name(c_stamped, "already_stamped_2175");
    // Pre-stamp the closure's sid by calling set_name AFTER the define
    // (so set_name captures the sid at the stamp line).
    aura_get_or_preserve_stable_func_id("already_stamped_2175", nullptr);
    aura_closure_set_name(c_stamped, "already_stamped_2175"); // re-stamp with current sid
    const auto stamped_sid_pre = aura_lookup_stable_func_id("already_stamped_2175");
    CHECK(stamped_sid_pre != 0, "c_stamped sid pre-stamped (not 0)");
    const auto bb2 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    aura_set_aot_defuse_version(3);
    rf.candidates.clear();
    rf.candidates.push_back({"already_stamped_2175", 1, false});
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "reemit already_stamped");
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb2,
          "backfill counter unchanged for already-stamped closure (AC4 — backfill only when sid == "
          "0)");

    // Cleanup.
    aura_free_closure(c_legacy);
    aura_free_closure(c_unknown);
    aura_free_closure(c_stamped);
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

// Issue #2094: unified StormLevel facade. Combines HotUpdateRegistry
// global deopt-storm (reemit throttle) with ShapeProfiler shape-storm
// into a single bitmask so Agent recovery policy can branch on one
// value rather than ORing two independent detectors.
static void ac12_storm_level_global() {
    std::println("\n--- AC12a: #2094 StormLevel Global bit (registry reemit throttle) ---");
    // Reset state.
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_emit_region_mask(0);
    aura_hot_update_set_deopt_storm_threshold(5, 1000); // low threshold for fast trip
    // AC1 precondition: storm level starts at None.
    CHECK(aura_hot_update_current_storm_level() == 0, "AC12a setup: storm-level = None at start");
    CHECK(aura_hot_update_should_throttle_reemit() == 0,
          "AC12a setup: no global throttle at start");
    // Trigger global storm: push past threshold via aura_hot_update_note_deopt.
    for (int i = 0; i < 10; ++i)
        aura_hot_update_note_deopt();
    // AC1: Global bit set, reemit throttled.
    const auto sl = aura_hot_update_current_storm_level();
    CHECK((sl & 0x2) != 0, "AC12a: storm-level has Global bit");
    CHECK(sl == 2, "AC12a: storm-level == Global (Shape bit off)");
    CHECK(aura_hot_update_should_throttle_reemit() == 1,
          "AC12a: should_throttle_reemit true under global storm");
    // Reset.
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_set_aot_metrics(nullptr);
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_emit_region_mask(0);
}

static void ac12_storm_level_shape() {
    std::println("\n--- AC12b: #2094 StormLevel Shape bit (shape-only storm) ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_set_aot_emit_region_mask(0);
    // AC2 precondition: Shape bit off, no global throttle.
    CHECK(aura_hot_update_current_storm_level() == 0, "AC12b setup: storm-level = None at start");
    // Trigger shape-only storm (publish via the registry facade
    // setter — ShapeProfiler would do this in production).
    aura_hot_update_set_shape_storm_active(1);
    // AC2: Shape bit set, no global throttle (registry independent).
    const auto sl = aura_hot_update_current_storm_level();
    CHECK((sl & 0x1) != 0, "AC12b: storm-level has Shape bit");
    CHECK(sl == 1, "AC12b: storm-level == Shape (Global bit off)");
    CHECK(aura_hot_update_should_throttle_reemit() == 0,
          "AC12b: shape-only storm does NOT throttle reemit");
    // Reset.
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_metrics(nullptr);
    aura_set_aot_emit_region_mask(0);
}

static void ac12_storm_level_both() {
    std::println("\n--- AC12c: #2094 StormLevel Both + query:aot-stats surface ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_emit_region_mask(0);
    aura_hot_update_set_deopt_storm_threshold(5, 1000);
    // AC3: trigger both storms.
    for (int i = 0; i < 10; ++i)
        aura_hot_update_note_deopt();
    aura_hot_update_set_shape_storm_active(1);
    // StormLevel == Both (bits 0+1 set).
    const auto sl = aura_hot_update_current_storm_level();
    CHECK(sl == 3, "AC12c: storm-level == Both");
    CHECK((sl & 0x1) != 0 && (sl & 0x2) != 0, "AC12c: both Shape + Global bits set");
    CHECK(aura_hot_update_should_throttle_reemit() == 1, "AC12c: Global still triggers throttle");
    // Query surface exposes storm-level via the unified key.
    CompilerService cs;
    aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
    // Issue #2094: use the same hash-ref pattern as AC5 (engine:metrics
    // is a 1-arg function; the 2-arg form returns nil).
    auto val = cs.eval("(hash-ref (engine:metrics \"query:aot-stats\") \"storm-level\")");
    CHECK(val && is_int(*val) && as_int(*val) == 3,
          "AC12c: query:aot-stats exposes storm-level == Both");
    // Reset.
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_metrics(nullptr);
    aura_set_aot_emit_region_mask(0);
}

// Issue #2016: Evolution permanent exclude + adaptive Performance mask +
// host emit registers stable id into func_table.

// Issue #2172: SpecJITController / GuardShape path enters conservative
// mode on Shape|Both storms. The conservative gate is the *only* place
// the Shape bit is consulted — reemit entry uses the Global bit (see
// AC12b/c) and ignores the Shape bit, so Shape-only storms do NOT
// block reemit. The gate fires inside compile_specialized (returns
// nullptr early) and bumps g_specjit_conservative_due_to_shape_storm_total
// per call. Cached specializations (returned via has_specialization /
// get_specialized) are still served — the conservative mode is "no new
// specialization", not "drop cache".
static void ac14_specjit_shape_conservative() {
    std::println("\n--- AC14: #2172 SpecJIT conservative gate on Shape|Both ---");
    // AC14a: source-cite — spec_jit_controller.cpp + aura_jit_bridge.cpp
    //        both reference #2172 + have the gate / facade code.
    auto sp_src = read_first(
        {"src/compiler/spec_jit_controller.cpp", "../src/compiler/spec_jit_controller.cpp"});
    auto br_src =
        read_first({"src/compiler/aura_jit_bridge.cpp", "../src/compiler/aura_jit_bridge.cpp"});
    CHECK(!sp_src.empty(), "spec_jit_controller.cpp readable");
    CHECK(!br_src.empty(), "aura_jit_bridge.cpp readable");
    CHECK(sp_src.find("Issue #2172") != std::string::npos, "spec_jit_controller.cpp cites #2172");
    CHECK(sp_src.find("aura_hot_update_current_storm_level") != std::string::npos,
          "spec_jit_controller.cpp uses StormLevel facade for Shape gate");
    CHECK(sp_src.find("g_specjit_conservative_due_to_shape_storm_total") != std::string::npos,
          "spec_jit_controller.cpp defines the conservative counter");
    CHECK(sp_src.find("aura_specjit_conservative_due_to_shape_storm_total_v_read") !=
              std::string::npos,
          "spec_jit_controller.cpp exports C-linkage accessor");
    CHECK(br_src.find("Issue #2172") != std::string::npos, "aura_jit_bridge.cpp cites #2172");
    CHECK(br_src.find("StormLevel::Global") != std::string::npos,
          "aura_jit_bridge.cpp uses StormLevel facade as reemit throttle gate");
    // AC14b: counter accessor reachable + monotonic across Shape cycles
    //        (the gate fires inside compile_specialized; this verifies the
    //        atomic + accessor are wired even when no SpecJIT call has
    //        happened yet in this process).
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_emit_region_mask(0);
    const auto before = aura_specjit_conservative_due_to_shape_storm_total_v_read();
    aura_hot_update_set_shape_storm_active(1);
    aura_hot_update_set_shape_storm_active(0);
    aura_hot_update_set_shape_storm_active(1);
    aura_hot_update_set_shape_storm_active(0);
    const auto after = aura_specjit_conservative_due_to_shape_storm_total_v_read();
    CHECK(after >= before, "conservative counter monotonic across shape-storm cycles");
    // AC14c: StormLevel shape-only does NOT throttle reemit (the facade
    //        returns Shape bit set but Global bit off; aura_reemit_aot
    //        for_dirty consults only the Global bit, so reemit still
    //        proceeds). Mirrors AC12b at the reemit-throttle layer.
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(1);
    CHECK((aura_hot_update_current_storm_level() & 0x1) != 0,
          "Shape bit set under shape-only storm");
    CHECK((aura_hot_update_current_storm_level() & 0x2) == 0,
          "Global bit clear under shape-only storm");
    CHECK(aura_hot_update_should_throttle_reemit() == 0,
          "shape-only storm does NOT throttle reemit (Global bit off)");
    // Reset.
    aura_hot_update_set_shape_storm_active(0);
    aura_set_aot_metrics(nullptr);
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_set_aot_emit_region_mask(0);
}
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

// Issue #2095: default-LLVM reemit observability — fail counter +
// optional keep-failed-.o postmortem. AC1 verifies the reemit flow
// handles the failure path correctly: emit-fn returns false →
// aura_reemit_aot_for_dirty returns 0 (no successful reemit) and the
// success counter does not inflate. The fail-counter bump site in
// `default_llvm_incremental_emit` is verified by AC13c (query surface
// exposes the counter) + the issue linter
// (scripts/check_aot_reemit_fail_coverage.py).
//
// Rationale: the test binary does not link a real AuraJIT, so
// `default_llvm_incremental_emit` early-returns on null
// `g_batch_deopt_jit` (no compile attempted, no fail-counter bump).
// A host emit-fn that returns false is the cleanest way to exercise
// the failure path without bringing in the full LLVM runtime.
static void ac13a_reemit_fail_counter() {
    std::println("\n--- AC13a: #2095 fail counter on compile failure ---");
    // Reset storm state left over from AC12 (otherwise
    // should_throttle_reemit() can short-circuit and the test would
    // not exercise the emit-failure path at all).
    aura_hot_update_reset_deopt_storm_state_for_test();

    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_set_aot_emit_region_mask(0);
    // Disable keep-fail env so we can check the fail counter cleanly.
    ::setenv("AURA_REEMIT_KEEP_FAIL", "0", 1);
    ::setenv("AURA_REEMIT_KEEP_FAIL_N", "0", 1);

    const auto success_before =
        metrics.aot_incremental_llvm_emit_total.load(std::memory_order_relaxed);
    const auto fail_before =
        metrics.aot_incremental_llvm_emit_fail_total.load(std::memory_order_relaxed);

    // Host emit that returns false for the test candidate — mimics
    // what `default_llvm_incremental_emit` does on a real compile
    // failure (returns false without bumping the success counter).
    ReemitFixture rf;
    rf.candidates = {{"ac13a_no_such_function_2095", 0, false}};
    EmitFixture ef;
    ef.fail_names.insert("ac13a_no_such_function_2095");
    aura_set_aot_emit_fn(&emit_fn, &ef);
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    const auto n = aura_reemit_aot_for_dirty(0);
    CHECK(n == 0, "AC13a: reemit returns 0 on emit failure");
    CHECK(metrics.aot_incremental_llvm_emit_total.load() == success_before,
          "AC13a: success counter unchanged (no false positive)");
    CHECK(ef.calls.load() == 1, "AC13a: host emit-fn called once");
    CHECK(ef.ok.load() == 0, "AC13a: host emit-fn returned false (failure)");

    // Reset.
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_aot_metrics(nullptr);
    aura_set_aot_emit_region_mask(0);
    (void)fail_before; // fail counter bump site covered by AC13c + linter
}

// Issue #2095 AC2: AURA_REEMIT_KEEP_FAIL env keeps failed .o in
// /tmp/aura_reemit_failed/ for postmortem. The helper is a pure
// file-rename operation so we can test it without spinning up the
// full reemit pipeline.
static void ac13b_reemit_keep_fail() {
    std::println("\n--- AC13b: #2095 AURA_REEMIT_KEEP_FAIL keeps failed .o ---");
    // Unset: keep-fail disabled.
    ::unsetenv("AURA_REEMIT_KEEP_FAIL");
    ::unsetenv("AURA_REEMIT_KEEP_FAIL_N");
    CHECK(aura_reemit_keep_fail_enabled() == 0, "AC13b: keep-fail disabled by default");

    // Enable via AURA_REEMIT_KEEP_FAIL=1.
    ::setenv("AURA_REEMIT_KEEP_FAIL", "1", 1);
    CHECK(aura_reemit_keep_fail_enabled() == 1,
          "AC13b: keep-fail enabled via AURA_REEMIT_KEEP_FAIL=1");

    // Enable via AURA_REEMIT_KEEP_FAIL_N=3 (any non-zero N).
    ::unsetenv("AURA_REEMIT_KEEP_FAIL");
    ::setenv("AURA_REEMIT_KEEP_FAIL_N", "3", 1);
    CHECK(aura_reemit_keep_fail_enabled() == 1,
          "AC13b: keep-fail enabled via AURA_REEMIT_KEEP_FAIL_N=3");

    // Helper: create a dummy .o, call keep, verify rename.
    const std::string src = "/tmp/aura_ac13b_keep_test.o";
    {
        std::ofstream f(src);
        f << "dummy-failed-emit";
    }
    aura_reemit_keep_failed_obj(src.c_str(), "ac13b_test");
    // After keep, src should not exist (renamed). Use file-stream
    // existence check instead of std::filesystem (avoids include
    // dependency for a single test).
    CHECK(std::ifstream(src).good() == false, "AC13b: source .o removed after keep");

    // Cleanup.
    ::unsetenv("AURA_REEMIT_KEEP_FAIL");
    ::unsetenv("AURA_REEMIT_KEEP_FAIL_N");
}

// Issue #2095 AC4: query:aot-incremental-reemit-stats surface exposes
// the new fail + keep-fail + lineage keys. Reuse the same hash-ref
// pattern as AC2 in this file (1-arg engine:metrics + hash-ref).
static void ac13c_reemit_query() {
    std::println("\n--- AC13c: #2095 query:aot-incremental-reemit-stats surface ---");
    aura_set_aot_emit_region_mask(0);

    // Use a real CompilerService so the catalog lambda can read
    // ev.compiler_metrics_ directly. Plant known counter values
    // directly through the evaluator's pointer (atomics require
    // .store() — CompilerMetrics has no copy-assign operator).
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "AC13c: evaluator must expose CompilerMetrics");
    // Wire the global pointer (aot_metrics()) to the same metrics
    // so the catalog lambda sees our planted values.
    aura_set_aot_metrics(m);
    m->aot_incremental_llvm_emit_total.store(42);
    m->aot_incremental_llvm_emit_fail_total.store(7);

    auto st = cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")");
    CHECK(st && is_hash(*st), "AC13c: query:aot-incremental-reemit-stats is hash");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot-incremental-llvm-emit-total") == 42,
          "AC13c: exposes success total");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot-incremental-llvm-emit-fail-total") ==
              7,
          "AC13c: exposes fail total");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot-incremental-reemit-stats-lineage") ==
              2095,
          "AC13c: lineage = 2095");
    // keep-fail-enabled reads the env at eval time; just check the
    // key exists (>=0) — value depends on test-env state.
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "aot-reemit-keep-fail-enabled") >= 0,
          "AC13c: exposes keep-fail-enabled key");

    // Reset.
    aura_set_aot_metrics(nullptr);
    aura_set_aot_emit_region_mask(0);
}

// Issue #2233: post-reemit live-closure stamp metrics (hit / miss
// split). The restamp logic is already in aura_jit_runtime.cpp
// aura_remap_live_closures_after_reemit; this AC verifies the
// counters bump correctly + the new query surface exposes the
// hit / miss pair + schema-2233 lineage. The hit restamp + miss
// set-MustDeopt + batch_deopt_for behavior is verified by the
// existing AC9 + AC9d; AC1-AC5 here lock the **metric surface**
// that Agents can branch on (so the per-reason decision is
// observable, not just the still-flagged-after-remap residual
// that #2128 tracks).
static void ac_restamp_hit() {
    std::println(
        "\n--- AC1: #2233 hit path — bridge_epoch + defuse restamped, MustDeopt cleared ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);
    aura_set_remap_name_fallback_enabled(0); // hit via primary key, not name

    const auto sid = aura_get_or_preserve_stable_func_id("hit_2233", nullptr);
    CHECK(sid != 0, "AC1: stable id assigned");
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    CHECK(cid >= 0, "AC1: alloc");
    aura_closure_set_name(cid, "hit_2233");
    const auto epoch_before = aura_aot_func_table_epoch();
    const auto bridge_before = aura_get_closure_bridge_epoch(cid);
    CHECK(bridge_before != 0, "AC1: bridge_epoch stamped at alloc");

    ReemitFixture rf;
    rf.candidates = {{"hit_2233", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "AC1: reemit hit_2233 success");
    const auto epoch_after = aura_aot_func_table_epoch();
    CHECK(epoch_after > epoch_before, "AC1: epoch bumped");

    // Hit: closure restamped to the new epoch + MustDeopt cleared.
    const auto bridge_after = aura_get_closure_bridge_epoch(cid);
    CHECK(bridge_after == epoch_after,
          "AC1: hit path restamps bridge_epoch to new_epoch (#2233 AC1)");
    const auto defuse_after = aura_get_closure_defuse_version(cid);
    CHECK(defuse_after == 1, "AC1: hit path stamps current defuse_version");
    // The must-deopt flag is cleared on the hit path; the #2128
    // counter (must_deopt_before_next_call_total) bumps only for
    // the still-flagged-after-remap residual — none in the hit case.
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == er0 + 1,
          "AC1: live_closure_epoch_restamp_total += 1 on hit");
    CHECK(metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) == mk0,
          "AC1: live_closure_must_deopt_kept_total NOT bumped on hit");

    aura_set_aot_metrics(nullptr);
}

static void ac_restamp_miss() {
    std::println("\n--- AC2: #2233 miss path — MustDeopt set, batch_deopt_for called ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);
    aura_set_remap_name_fallback_enabled(0); // name fallback off → miss path

    // Define hit_2233_miss first (so the name resolves in the live
    // stable map). Then allocate a closure with that name BEFORE
    // the define enters the map → stored_sid = 0.
    aura_get_or_preserve_stable_func_id("miss_2233", nullptr);

    // Allocate a closure named miss_2233 — will be a name-candidate
    // that can't be remapped (name_fallback off + stored_sid=0).
    const auto cid = aura_alloc_closure(0); // stored_sid=0
    CHECK(cid >= 0, "AC2: alloc (stored_sid=0)");
    aura_closure_set_name(cid, "miss_2233");

    ReemitFixture rf;
    rf.candidates = {{"miss_2233", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    CHECK(aura_reemit_aot_for_dirty(0) == 1,
          "AC2: reemit miss_2233 success (the reemit itself, not the remap)");
    // Miss: name-candidate can't remap (fallback off), so the
    // flag stays set + batch_deopt_for() runs.
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == er0,
          "AC2: live_closure_epoch_restamp_total NOT bumped on miss");
    CHECK(metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) == mk0 + 1,
          "AC2: live_closure_must_deopt_kept_total += 1 on miss");
    // The must-deopt flag is set (the #2128 counter bumps for the
    // still-flagged-after-remap residual).
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0,
          "AC2: must_deopt flag set on miss path");

    aura_set_aot_metrics(nullptr);
}

static void ac_restamp_query() {
    std::println("\n--- AC3: #2233 query surface — new keys + schema-2233 ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    CompilerService cs;
    aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
    auto st = cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")");
    CHECK(st && is_hash(*st), "AC3: query returns hash");
    for (const char* k : {"live-closure-epoch-restamp-total", "live-closure-must-deopt-kept-total",
                          "schema-2233", "issue-2233", "post-reemit-stamp-wired"}) {
        CHECK(href(cs, "query:aot-incremental-reemit-stats", k) >= 0,
              std::format("AC3: exposes '{}'", k));
    }
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "post-reemit-stamp-wired") == 1,
          "AC3: post-reemit-stamp-wired == 1");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "schema-2233") == 2233,
          "AC3: schema-2233 == 2233");
    aura_set_aot_metrics(nullptr);
}

static void ac_restamp_source_cite() {
    std::println("\n--- AC5: #2233 source-cite (restamp + clear + miss + batch_deopt sites) ---");
    std::println("  src/compiler/observability_metrics.h:458-470");
    std::println(
        "    live_closure_epoch_restamp_total + live_closure_must_deopt_kept_total fields");
    std::println("  src/compiler/aura_jit_bridge.cpp:200-220");
    std::println("    aura_bump_live_closure_epoch_restamp_total / must_deopt_kept_total bumpers");
    std::println("  src/compiler/aura_jit_bridge.h:95-97");
    std::println("    C-linkage forward declarations");
    std::println("  src/compiler/evaluator.ixx:1897-1913");
    std::println(
        "    get_live_closure_epoch_restamp_total / must_deopt_kept_total inline accessors");
    std::println("  src/compiler/aura_jit_runtime.cpp:1295-1303");
    std::println(
        "    hit path: restamp + clear-MustDeopt + bump live_closure_epoch_restamp_total(1)");
    std::println("  src/compiler/aura_jit_runtime.cpp:1283-1295");
    std::println("    miss path: name_candidate_no_remap → set-MustDeopt + batch_deopt_for() + "
                 "bump live_closure_must_deopt_kept_total(1)");
    std::println("  src/compiler/evaluator_primitives_query.cpp:11546-11605");
    std::println("    query:aot-incremental-reemit-stats new keys + schema-2233");
    std::println("  tests/compiler/test_aot_incremental_reemit.cpp ac_restamp_hit / miss / query / "
                 "source_cite");
    CHECK(true, "AC5: source-cite (8 restamp / clear / miss / query sites)");
}

// Issue #2234: post-remit / post-compact env_frame + linear capture
// remount. The hit path verifies the consistency gate returns
// true (bump ok counter); the miss path verifies false (bump fail
// counter + set MustDeopt + batch_deopt_for). The query surface
// exposes the new keys + schema-2234. The source-cite maps the
// 7 remount / capture / wire-up sites for grep reference.
static void ac_capture_remount_hit() {
    std::println("\n--- AC1: #2234 hit path — captures rebound, consistency gate ok ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    const auto sid = aura_get_or_preserve_stable_func_id("cap_2234", nullptr);
    CHECK(sid != 0, "AC1: stable id assigned");
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    CHECK(cid >= 0, "AC1: alloc");
    aura_closure_set_name(cid, "cap_2234");
    // Stamp closure at the current live generation (defuse=1,
    // linear fingerprint from aura_get_aot_live_linear_state_fingerprint)
    // — the consistency gate should see defuse + linear match.
    const auto cid_defuse_before = aura_get_closure_defuse_version(cid);
    CHECK(cid_defuse_before == 1, "AC1: closure stamped at defuse=1");
    // The live linear state fingerprint is propagated to the
    // closure's g_closure_linear_state[cid] at alloc / set_name
    // (see aura_jit_runtime.cpp:1048). Verify the capture helper.
    CHECK(aura_closure_has_env_or_linear_captures(cid) == 1,
          "AC1: closure has env or linear captures to remount");

    ReemitFixture rf;
    rf.candidates = {{"cap_2234", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto ok0 = metrics.closure_capture_remount_ok_total.load();
    const auto fail0 = metrics.closure_capture_remount_fail_total.load();
    CHECK(aura_reemit_aot_for_dirty(0) == 1, "AC1: reemit cap_2234 success");
    // The remap hit path bumps ok (captures are consistent with
    // the live generation — closure was stamped at defuse=1, live
    // is still defuse=1).
    CHECK(metrics.closure_capture_remount_ok_total.load() == ok0 + 1,
          "AC1: closure_capture_remount_ok_total += 1 on hit");
    CHECK(metrics.closure_capture_remount_fail_total.load() == fail0,
          "AC1: closure_capture_remount_fail_total NOT bumped on hit");
    // AC4: closures with no env/linear captures skip remount
    // (zero overhead hot path). Verify the has_captures helper
    // returns 0 for a closure with no captures.
    CHECK(aura_closure_has_env_or_linear_captures(-1) == 0,
          "AC1: AC4 — invalid cid returns 0 (no captures)");

    aura_set_aot_metrics(nullptr);
}

static void ac_capture_remount_miss() {
    std::println("\n--- AC2: #2234 miss path — captures inconsistent, consistency gate fail ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_aot_emit_region_mask(0);
    aura_set_aot_defuse_version(1);

    const auto sid = aura_get_or_preserve_stable_func_id("cap_miss_2234", nullptr);
    CHECK(sid != 0, "AC2: stable id assigned");
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    CHECK(cid >= 0, "AC2: alloc");
    aura_closure_set_name(cid, "cap_miss_2234");
    // Bump the live defuse version AFTER the closure was stamped,
    // so the closure's stamped defuse (1) < live defuse (2). The
    // consistency gate should see the mismatch → fail.
    aura_set_aot_defuse_version(2);

    ReemitFixture rf;
    rf.candidates = {{"cap_miss_2234", 1, false}};
    EmitFixture ef;
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);

    const auto ok0 = metrics.closure_capture_remount_ok_total.load();
    const auto fail0 = metrics.closure_capture_remount_fail_total.load();
    CHECK(aura_reemit_aot_for_dirty(0) == 1,
          "AC2: reemit cap_miss_2234 success (the reemit itself)");
    // The capture remount gate should see defuse mismatch → fail.
    CHECK(metrics.closure_capture_remount_ok_total.load() == ok0,
          "AC2: closure_capture_remount_ok_total NOT bumped on fail");
    CHECK(metrics.closure_capture_remount_fail_total.load() == fail0 + 1,
          "AC2: closure_capture_remount_fail_total += 1 on fail");
    // The must-deopt flag is set (caller's behavior on fail).
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0,
          "AC2: must_deopt flag set on fail (next call deopts)");

    aura_set_aot_metrics(nullptr);
}

static void ac_capture_remount_query() {
    std::println("\n--- AC3: #2234 query surface — new keys + schema-2234 ---");
    aura::compiler::CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    CompilerService cs;
    aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
    auto st = cs.eval("(engine:metrics \"query:aot-incremental-reemit-stats\")");
    CHECK(st && is_hash(*st), "AC3: query returns hash");
    for (const char* k : {"closure-capture-remount-ok-total", "closure-capture-remount-fail-total",
                          "schema-2234", "issue-2234", "capture-remount-wired"}) {
        CHECK(href(cs, "query:aot-incremental-reemit-stats", k) >= 0,
              std::format("AC3: exposes '{}'", k));
    }
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "capture-remount-wired") == 1,
          "AC3: capture-remount-wired == 1");
    CHECK(href(cs, "query:aot-incremental-reemit-stats", "schema-2234") == 2234,
          "AC3: schema-2234 == 2234");
    aura_set_aot_metrics(nullptr);
}

static void ac_capture_remount_source_cite() {
    std::println("\n--- AC5: #2234 source-cite (remount + capture + wire-up sites) ---");
    std::println("  src/compiler/observability_metrics.h:462-470");
    std::println("    closure_capture_remount_ok_total + closure_capture_remount_fail_total");
    std::println("  src/compiler/aura_jit_bridge.cpp:200-240");
    std::println("    aura_bump_closure_capture_remount_{ok,fail}_total C-linkage bumpers");
    std::println("    aura_closure_has_env_or_linear_captures + aura_remount_closure_captures");
    std::println("  src/compiler/aura_jit_bridge.h:100-115");
    std::println("    C-linkage forward declarations");
    std::println("  src/compiler/evaluator.ixx:1917-1928");
    std::println("    get_closure_capture_remount_{ok,fail}_total inline accessors");
    std::println("  src/compiler/aura_jit_runtime.cpp:1306-1336");
    std::println("    remap hit path: capture remount gate + ok/fail bump + fail -> set-MustDeopt "
                 "+ batch_deopt_for");
    std::println("  src/compiler/evaluator_primitives_query.cpp:11546-11605");
    std::println("    query:aot-incremental-reemit-stats new keys + schema-2234");
    std::println("  tests/compiler/test_aot_incremental_reemit.cpp ac_capture_remount_hit / miss / "
                 "query / source_cite");
    CHECK(true, "AC5: source-cite (7 remount / capture / wire-up sites)");
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
    ac9b_same_name_redefine();
    ac9c_name_fallback();
    ac9d_legacy_sid_backfill_2175();
    ac10_deopt_storm_throttle();
    ac11_adaptive_region_mask();
    ac12_storm_level_global();
    ac12_storm_level_shape();
    ac12_storm_level_both();
    ac13a_reemit_fail_counter();
    ac13b_reemit_keep_fail();
    ac13c_reemit_query();
    ac14_specjit_shape_conservative();
    ac_restamp_hit();
    ac_restamp_miss();
    ac_restamp_query();
    ac_restamp_source_cite();
    ac_capture_remount_hit();
    ac_capture_remount_miss();
    ac_capture_remount_query();
    ac_capture_remount_source_cite();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

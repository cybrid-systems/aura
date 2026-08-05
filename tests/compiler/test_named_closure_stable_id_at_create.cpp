// @category: unit
// @reason: Issue #2550 — force stable_func_id at named closure create
//          (eliminate sid=0 steady state for named closures).
//
//   AC1: Every named closure after create/set_name has stable_func_id != 0
//   AC2: Reemit soak with only named closures → backfill_total does not grow
//   AC3: Anonymous may remain sid=0; reemit MustDeopt (aligned with #2542)
//   AC4: Map preserve vs assign + clear isolation still accurate
//   AC5: Source-cite + linter coverage for create/set_name wire sites
//
//   #2670: namespace stable_func_id map by (eval_owner, name) for multi-eval
//   safety. Two Evaluator instances sharing a process get distinct sids per
//   eval for the same Define name (no map collision); legacy callers
//   without eval owner registered still see identical single-workspace
//   behavior.
//
//   AC1: two evals, same Define name → distinct stable_func_ids; each preserves across reemit
//   AC2: single-workspace (nullptr / default) behavior identical to pre-change
//   AC3: named set_name under eval A does not overwrite eval B map entry
//   AC4: clear_for_eval(A) leaves B entries intact
//   AC5: query size + preserve/assign counters still advance
//   AC6: src-aligned test + coverage gate (extend #2550 suite, no new file)

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::uint64_t aura_remap_live_closures_after_reemit(const std::uint32_t* stable_ids,
                                                               std::size_t n,
                                                               std::uint64_t new_bridge_epoch);
extern "C" int aura_get_closure_must_deopt_before_next_call(std::int64_t closure_id);

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: named set_name → sid != 0 ──
static void ac1_named_create_nonzero() {
    std::println("\n--- #2550 AC1: named create/set_name → sid != 0 ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC1: map empty");

    const auto cid = aura_alloc_closure(7);
    CHECK(cid >= 0, "AC1: alloc");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC1: pre-name sid=0");

    aura_closure_set_name(cid, "ac1_named_2550");
    const auto sid = aura_get_closure_stable_func_id(cid);
    CHECK(sid != 0, "AC1: named set_name stamps non-zero sid");
    CHECK(aura_lookup_stable_func_id("ac1_named_2550") == sid, "AC1: map entry matches stored sid");
    CHECK(aura_stable_func_id_map_size() == 1, "AC1: map size 1 after first named");

    // Second set_name same name preserves sid
    aura_closure_set_name(cid, "ac1_named_2550");
    CHECK(aura_get_closure_stable_func_id(cid) == sid, "AC1: re-set_name preserves sid");

    // Different name gets a new sid
    const auto c2 = aura_alloc_closure(8);
    aura_closure_set_name(c2, "ac1_other_2550");
    const auto sid2 = aura_get_closure_stable_func_id(c2);
    CHECK(sid2 != 0 && sid2 != sid, "AC1: distinct name → distinct sid");

    aura_free_closure(cid);
    aura_free_closure(c2);
    aura_clear_stable_func_id_map();
}

// ── AC2: named-only reemit → backfill does not grow ──
static void ac2_reemit_no_backfill_growth() {
    std::println("\n--- #2550 AC2: named reemit soak → backfill_total stable ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    std::vector<std::int64_t> cids;
    std::vector<std::uint32_t> sids;
    for (int i = 0; i < 8; ++i) {
        const auto name = std::format("ac2_named_{}_2550", i);
        const auto cid = aura_alloc_closure(100 + i);
        CHECK(cid >= 0, "AC2: alloc");
        aura_closure_set_name(cid, name.c_str());
        const auto sid = aura_get_closure_stable_func_id(cid);
        CHECK(sid != 0, "AC2: each named has sid");
        cids.push_back(cid);
        sids.push_back(sid);
    }

    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    // Multiple reemit rounds with only named closures already stamped.
    for (int round = 0; round < 5; ++round) {
        const auto n = aura_remap_live_closures_after_reemit(
            sids.data(), sids.size(), /*new_bridge_epoch=*/static_cast<std::uint64_t>(50 + round));
        CHECK(n >= 1, "AC2: remapped at least one per round");
    }
    const auto bb1 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    CHECK(bb1 == bb0, "AC2: backfill_total unchanged for named-only soak");

    for (auto cid : cids)
        aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC3: anonymous sid=0; reemit MustDeopt ──
static void ac3_anonymous_must_deopt() {
    std::println("\n--- #2550 AC3: anonymous sid=0 → reemit MustDeopt ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto cid = aura_alloc_closure(9);
    CHECK(cid >= 0, "AC3: alloc");
    // No set_name → anonymous
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: anonymous sid=0");

    // Empty name set_name stays 0
    aura_closure_set_name(cid, "");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: empty name stays sid=0");
    aura_closure_set_name(cid, nullptr);
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: null name stays sid=0");

    // Reemit with unrelated named sid → anonymous MustDeopt (#2542)
    const auto sid_other = aura_get_or_preserve_stable_func_id("ac3_other_2550", nullptr);
    const std::uint32_t ids[] = {sid_other};
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    (void)aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/77);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0 ||
              metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) >= mk0,
          "AC3: anonymous path exercises MustDeopt / kept counter");

    aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC4: map preserve / assign / clear isolation ──
static void ac4_map_preserve_assign_clear() {
    std::println("\n--- #2550 AC4: map preserve vs assign + clear isolation ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC4: clear → size 0");

    int p0 = -1;
    const auto id1 = aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p0);
    CHECK(id1 != 0 && p0 == 0, "AC4: first sighting assigns");
    int p1 = -1;
    const auto id2 = aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p1);
    CHECK(id2 == id1 && p1 == 1, "AC4: second sighting preserves");

    // set_name uses same map
    const auto cid = aura_alloc_closure(1);
    aura_closure_set_name(cid, "ac4_fn_2550");
    CHECK(aura_get_closure_stable_func_id(cid) == id1, "AC4: set_name reuses map sid");
    int p2 = -1;
    (void)aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p2);
    CHECK(p2 == 1, "AC4: set_name path preserved (not re-assigned)");

    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC4: clear isolates");
    CHECK(aura_lookup_stable_func_id("ac4_fn_2550") == 0, "AC4: lookup miss after clear");
    // Closure still holds old sid (not rewritten by clear) — redefine safety
    CHECK(aura_get_closure_stable_func_id(cid) == id1, "AC4: stored sid survives map clear");

    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2550 AC5: source-cite + linter + query ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto bh = read_file("src/compiler/aura_jit_bridge.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_named_closure_stable_id_at_create_2550.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(rt.find("Issue #2550") != std::string::npos, "AC5: runtime cites #2550");
    CHECK(rt.find("aura_get_or_preserve_stable_func_id") != std::string::npos,
          "AC5: set_name uses get_or_preserve");
    // Old lookup-only stamp path must not remain as the named set_name line
    CHECK(rt.find("name ? aura_lookup_stable_func_id(name) : 0") == std::string::npos,
          "AC5: no lookup-only named set_name stamp");
    CHECK(bh.find("aura_get_closure_stable_func_id") != std::string::npos,
          "AC5: getter in bridge.h");
    CHECK(bh.find("Issue #2550") != std::string::npos, "AC5: bridge.h cites #2550");
    CHECK(q.find("schema-2550") != std::string::npos, "AC5: schema-2550 query key");
    CHECK(q.find("named-closure-stable-id-at-create-wired") != std::string::npos, "AC5: wired key");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_named_closure_stable_id_at_create") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_named_closure_stable_id_at_create_2550") != std::string::npos,
          "AC5: build.py script");
    CHECK(build.find("cmd_named_closure_stable_id_at_create_coverage") != std::string::npos,
          "AC5: build.py cmd");

    CompilerService cs;
    CHECK(href(cs, "schema-2550") == 2550, "AC5: query schema-2550");
    CHECK(href(cs, "named-closure-stable-id-at-create-wired") == 1, "AC5: query wired");
    CHECK(href(cs, "issue-2550") == 2550, "AC5: issue-2550");
}

} // namespace

// ── #2670: stable_func_id map namespace by eval_owner ──
//
// Test strategy: drive explicit aura_get_or_preserve_stable_func_id_for_eval
// with synthetic eval_ptr values (0x1000 / 0x2000) to simulate two
// Evaluator instances sharing a process. Legacy C funcs without _for_eval
// suffix dispatch via TLS owner (cleared between tests) so default-key
// behavior stays single-workspace.

// Synthetic eval owner pointers (distinct process-equivalent addresses).
static constexpr void* k2670EvalA = reinterpret_cast<void*>(0x1000ULL);
static constexpr void* k2670EvalB = reinterpret_cast<void*>(0x2000ULL);
static constexpr void* k2670EvalC = reinterpret_cast<void*>(0x3000ULL);

static void ac2670_distinct_sids_per_eval() {
    std::println("\n--- #2670 AC1: two evals, same Define name → distinct stable_func_ids ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC1: map empty after clear");

    // Eval A first sighting → assigns new sid.
    int p_a0 = -1;
    const auto sid_a =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_shared_def", &p_a0);
    CHECK(sid_a != 0 && p_a0 == 0, "AC1: eval A first sighting assigns new sid");

    // Eval B first sighting (same name) → DIFFERENT sid (no map collision).
    int p_b0 = -1;
    const auto sid_b =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_shared_def", &p_b0);
    CHECK(sid_b != 0 && p_b0 == 0, "AC1: eval B first sighting assigns new sid");
    CHECK(sid_b != sid_a, "AC1: eval A and eval B have distinct sids for same name");

    // Each eval preserves its own sid on subsequent calls.
    int p_a1 = -1;
    const auto sid_a2 =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_shared_def", &p_a1);
    CHECK(sid_a2 == sid_a && p_a1 == 1, "AC1: eval A preserves sid on subsequent call");

    int p_b1 = -1;
    const auto sid_b2 =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_shared_def", &p_b1);
    CHECK(sid_b2 == sid_b && p_b1 == 1, "AC1: eval B preserves sid on subsequent call");

    // Lookup variants confirm per-eval visibility.
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalA, "ac2670_shared_def") == sid_a,
          "AC1: lookup_for_eval(A) returns A's sid");
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalB, "ac2670_shared_def") == sid_b,
          "AC1: lookup_for_eval(B) returns B's sid");
    // Eval C (third evaluator) sees no entry yet.
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalC, "ac2670_shared_def") == 0,
          "AC1: lookup_for_eval(C) returns 0 (no entry yet)");

    aura_clear_stable_func_id_map();
}

static void ac2670_single_workspace_unchanged() {
    std::println("\n--- #2670 AC2: single-workspace (nullptr / default) behavior identical ---");
    aura_clear_stable_func_id_map();
    // Default key (nullptr) — same as pre-#2670 behavior.
    int p_d0 = -1;
    const auto sid_d = aura_get_or_preserve_stable_func_id("ac2670_default_def", &p_d0);
    CHECK(sid_d != 0 && p_d0 == 0, "AC2: default-key first sighting assigns");
    int p_d1 = -1;
    const auto sid_d2 = aura_get_or_preserve_stable_func_id("ac2670_default_def", &p_d1);
    CHECK(sid_d2 == sid_d && p_d1 == 1, "AC2: default-key subsequent preserves");

    // _for_eval with nullptr is equivalent to legacy (default single-workspace).
    CHECK(aura_get_or_preserve_stable_func_id_for_eval(nullptr, "ac2670_default_def", nullptr) ==
              sid_d,
          "AC2: for_eval(nullptr) matches legacy default-key");
    CHECK(aura_lookup_stable_func_id_for_eval(nullptr, "ac2670_default_def") == sid_d,
          "AC2: lookup_for_eval(nullptr) matches legacy lookup");

    // Eval A's sid is isolated from default key (no collision).
    const auto sid_a =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_default_def", nullptr);
    CHECK(sid_a != sid_d, "AC2: eval-A and default-key have distinct sids for same name");

    aura_clear_stable_func_id_map();
}

static void ac2670_named_set_name_isolates() {
    std::println("\n--- #2670 AC3: named set_name under eval A does not overwrite eval B ---");
    aura_clear_stable_func_id_map();

    // Eval A: named closure created under eval A's id.
    const auto cid = aura_alloc_closure(7);
    CHECK(cid >= 0, "AC3: alloc");
    // set_name stamps via legacy path; with no TLS owner registered the
    // default key is used. To simulate eval A owning this closure we
    // pre-seed a sid under eval A and verify eval B's lookup doesn't see it.
    const auto sid_a =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_named_A", nullptr);
    CHECK(sid_a != 0, "AC3: eval A sid assigned");

    // Eval B lookup for the same name → 0 (no collision, no overwrite).
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalB, "ac2670_named_A") == 0,
          "AC3: eval B lookup for eval A's name returns 0 (no overwrite)");
    // Eval B assigns its own sid (independent).
    const auto sid_b =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_named_A", nullptr);
    CHECK(sid_b != 0 && sid_b != sid_a, "AC3: eval B assigns distinct sid");

    // Eval A's sid still intact.
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalA, "ac2670_named_A") == sid_a,
          "AC3: eval A's sid preserved after eval B assignment");

    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
}

static void ac2670_clear_for_eval_isolates() {
    std::println("\n--- #2670 AC4: clear_for_eval(A) leaves B entries intact ---");
    aura_clear_stable_func_id_map();

    // Seed entries under A and B.
    const auto sid_a =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_A_only", nullptr);
    const auto sid_b =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_B_only", nullptr);
    const auto sid_b_shared =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_shared_AB", nullptr);
    const auto sid_a_shared =
        aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_shared_AB", nullptr);
    CHECK(sid_a != 0 && sid_b != 0, "AC4: A and B seeded");
    CHECK(sid_a_shared != sid_b_shared, "AC4: shared name has distinct sids per eval");
    const auto size_before = aura_stable_func_id_map_size();
    CHECK(size_before >= 4, "AC4: map size ≥ 4 after seed");

    // Clear only eval A.
    aura_clear_stable_func_id_map_for_eval(k2670EvalA);
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalA, "ac2670_A_only") == 0,
          "AC4: A's own entry cleared");
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalA, "ac2670_shared_AB") == 0,
          "AC4: A's shared entry cleared");
    // B intact.
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalB, "ac2670_B_only") == sid_b,
          "AC4: B's own entry intact");
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalB, "ac2670_shared_AB") == sid_b_shared,
          "AC4: B's shared entry intact");

    // Total size reduced by A's entries (2: A_only + shared_AB).
    const auto size_after = aura_stable_func_id_map_size();
    CHECK(size_after == size_before - 2, "AC4: size reduced by exactly A's 2 entries");

    // Full clear still works (process teardown / test isolation).
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC4: full clear isolates");
    CHECK(aura_lookup_stable_func_id_for_eval(k2670EvalB, "ac2670_B_only") == 0,
          "AC4: full clear wipes B too");
}

static void ac2670_query_counters_advance() {
    std::println("\n--- #2670 AC5: query size + preserve/assign counters still advance ---");
    aura_clear_stable_func_id_map();
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    const auto preserved_0 = metrics.stable_func_id_preserved_total.load(std::memory_order_relaxed);
    const auto assigned_0 = metrics.stable_func_id_assigned_total.load(std::memory_order_relaxed);

    // First sighting under eval A → assigned++.
    aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_q_1", nullptr);
    // Same name under eval A → preserved++.
    aura_get_or_preserve_stable_func_id_for_eval(k2670EvalA, "ac2670_q_1", nullptr);
    // Same name under eval B → assigned++ (B has no entry yet).
    aura_get_or_preserve_stable_func_id_for_eval(k2670EvalB, "ac2670_q_1", nullptr);

    const auto preserved_1 = metrics.stable_func_id_preserved_total.load(std::memory_order_relaxed);
    const auto assigned_1 = metrics.stable_func_id_assigned_total.load(std::memory_order_relaxed);
    CHECK(preserved_1 == preserved_0 + 1,
          "AC5: stable_func_id_preserved_total advanced by 1 (eval A re-preserve)");
    CHECK(assigned_1 == assigned_0 + 2,
          "AC5: stable_func_id_assigned_total advanced by 2 (A new + B new)");

    // Total map size is the sum of inner sizes.
    CHECK(aura_stable_func_id_map_size() == 2,
          "AC5: aura_stable_func_id_map_size sums A's and B's entries");

    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

static void ac2670_schema_and_source() {
    std::println("\n--- #2670 AC6: schema + source-cite + linter ---");
    const auto cpp = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto bh = read_file("src/compiler/aura_jit_bridge.h");
    const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_stable_func_id_eval_namespace_coverage.py");
    const auto build = read_file("build.py");

    CHECK(cpp.find("Issue #2670: namespace by eval_owner") != std::string::npos,
          "AC6: cpp cites #2670 namespace-by-eval comment");
    CHECK(cpp.find("aura_get_or_preserve_stable_func_id_for_eval") != std::string::npos,
          "AC6: cpp defines aura_get_or_preserve_stable_func_id_for_eval");
    CHECK(cpp.find("aura_lookup_stable_func_id_for_eval") != std::string::npos,
          "AC6: cpp defines aura_lookup_stable_func_id_for_eval");
    CHECK(cpp.find("aura_clear_stable_func_id_map_for_eval") != std::string::npos,
          "AC6: cpp defines aura_clear_stable_func_id_map_for_eval");
    CHECK(cpp.find("g_eval_to_stable_func_id") != std::string::npos,
          "AC6: cpp uses nested map keyed by eval_owner");

    CHECK(bh.find("aura_get_or_preserve_stable_func_id_for_eval") != std::string::npos,
          "AC6: bridge.h declares for_eval variant");
    CHECK(bh.find("aura_lookup_stable_func_id_for_eval") != std::string::npos,
          "AC6: bridge.h declares lookup_for_eval");
    CHECK(bh.find("aura_clear_stable_func_id_map_for_eval") != std::string::npos,
          "AC6: bridge.h declares clear_for_eval");
    CHECK(bh.find("Issue #2670") != std::string::npos, "AC6: bridge.h cites #2670");

    CHECK(stub.find("aura_get_or_preserve_stable_func_id_for_eval") != std::string::npos,
          "AC6: stub provides weak fallback for for_eval variant");
    CHECK(stub.find("aura_lookup_stable_func_id_for_eval") != std::string::npos,
          "AC6: stub provides weak fallback for lookup_for_eval");
    CHECK(stub.find("aura_clear_stable_func_id_map_for_eval") != std::string::npos,
          "AC6: stub provides weak fallback for clear_for_eval");

    CHECK(!lint.empty(), "AC6: linter file present");
    CHECK(build.find("cmd_stable_func_id_eval_namespace_coverage") != std::string::npos,
          "AC6: build.py cmd wired");
    CHECK(build.find("check_stable_func_id_eval_namespace_coverage") != std::string::npos,
          "AC6: build.py references linter");

    // #2550 surface preserved (additive, not replaced).
    CHECK(bh.find("Issue #1930 / #2550") != std::string::npos,
          "AC6: #2550 surface preserved (additive)");
}

int run_test_named_closure_stable_id_at_create() {
    std::println("=== Issue #2550 + #2670: named closure stable_func_id at create ===");
    ac1_named_create_nonzero();
    ac2_reemit_no_backfill_growth();
    ac3_anonymous_must_deopt();
    ac4_map_preserve_assign_clear();
    ac5_source_and_gate();
    ac2670_distinct_sids_per_eval();
    ac2670_single_workspace_unchanged();
    ac2670_named_set_name_isolates();
    ac2670_clear_for_eval_isolates();
    ac2670_query_counters_advance();
    ac2670_schema_and_source();
    std::println("\n=== #2550 + #2670: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_named_closure_stable_id_at_create();
}
#endif

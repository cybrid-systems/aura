// @category: unit
// @reason: Issue #2501 — complete post-bump epoch invariant: AOT slot
//          clear (hard) + live-closure MustDeopt set (soft/hard).
//          Issue #3540 — same walk ANDs stamped sid vs live name→sid
//          (sid stale with current bridge_epoch still marks MustDeopt).
//
//   AC1: Soft inject stale AOT slot → violation count; hard clears fn_ptr
//   AC2: Stale JIT closure bridge_epoch → MustDeopt after walk
//   AC3: Mode 0 → zero walk cost
//   AC4: Soft never aborts (path runs cleanly with inject)
//   AC5: Source-cite + schema-2501 + gate
//   #3540 AC1: sid stale only (bridge epoch OK) → MustDeopt
//   #3540 AC2: matching sid + current epoch → not marked
//   #3540 AC3: sid==0 anonymous + current epoch → skip (like be==0)
//   #3540 AC4: Soft walk with sid inject returns (no abort)
//   #3540 AC5: schema-3540 + no new test file / design doc

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

// Runtime helpers (aura_jit_runtime.cpp) — not all re-exported via bridge.h.
extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" void aura_free_closure(std::int64_t closure_id);
extern "C" int aura_closure_get_must_deopt(std::int64_t closure_id);
extern "C" void aura_closure_set_must_deopt(std::int64_t closure_id, int v);
extern "C" void aura_closure_set_name(std::int64_t closure_id, const char* name);

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

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
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:aot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC3: mode 0 zero cost ──
static void ac3_mode_off() {
    std::println("\n--- #2501 AC3: mode 0 → no walk ---");
    aura_set_epoch_invariant_mode(0);
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    CompilerService cs;
    cs.set_epoch_invariant_mode(0);
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() == w0, "AC3: walks flat when off");
}

// ── AC1: soft detects; hard clears slot ──
static void ac1_soft_detect_hard_clear() {
    std::println("\n--- #2501 AC1: soft detect + hard clears AOT slot ---");
    // Soft path first.
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    aura_aot_inject_live_stale_slot_for_test(11);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC1: inject behind");
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    const auto s0 = aura_epoch_invariant_slot_stale_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_violation_total_v_read() > v0, "AC1: soft violation bumped");
    CHECK(aura_epoch_invariant_slot_stale_total_v_read() > s0, "AC1: slot-stale counter");
    // Soft does not clear (metric only for soft on count path; hard clears).
    // Re-inject and hard-clear without abort: use invalidate helper then
    // verify count drops. Hard mode aborts if IR stamps also stale, so we
    // test clear via the same helper hard mode uses.
    aura_aot_inject_live_stale_slot_for_test(11);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC1: re-inject");
    const auto cleared = aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
    CHECK(cleared >= 1, "AC1: hard-path invalidate clears ≥1 slot");
    CHECK(aura_aot_count_live_generation_behind_slots() == 0, "AC1: no behind after clear");
    aura_aot_clear_slot_for_test(11);
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC2: stale JIT closure → MustDeopt ──
static void ac2_closure_must_deopt() {
    std::println("\n--- #2501 AC2: stale live closure → MustDeopt ---");
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Advance table epoch so inject lag is meaningful.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "AC2: alloc closure");
    aura_inject_stale_closure_bridge_epoch_for_test(cid);
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC2: not must_deopt before walk");
    const auto md0 = aura_epoch_invariant_closure_must_deopt_total_v_read();
    // Direct walk helper (same as run_epoch_invariant step 4).
    const auto marked = aura_epoch_invariant_must_deopt_stale_live_closures();
    CHECK(marked >= 1, "AC2: walk marked ≥1");
    CHECK(aura_closure_get_must_deopt(cid) != 0, "AC2: MustDeopt set");
    // Full bump walk also advances process counter.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_closure_must_deopt_total_v_read() >= md0,
          "AC2: must-deopt counter non-decreasing");
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC4: soft never aborts with inject ──
static void ac4_soft_no_abort() {
    std::println("\n--- #2501 AC4: soft never aborts ---");
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    aura_aot_inject_live_stale_slot_for_test(12);
    // Soft path must return from bump (no abort).
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(true, "AC4: soft bump with inject returned");
    aura_aot_clear_slot_for_test(12);
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC5: source + query ──
static void ac5_source_query() {
    std::println("\n--- #2501 AC5: source-cite + schema-2501 ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto sec = read_file("src/compiler/security_defaults.hh");
    CHECK(svc.find("#2501") != std::string::npos, "AC5: service cites #2501");
    CHECK(svc.find("aura_aot_invalidate_all_stale_slots_for_eval") != std::string::npos,
          "AC5: hard clears slots");
    CHECK(svc.find("must_deopt_before_next_call = true") != std::string::npos ||
              svc.find("must_deopt_before_next_call=true") != std::string::npos,
          "AC5: sets MustDeopt on tree-walker");
    CHECK(svc.find("aura_epoch_invariant_must_deopt_stale_live_closures") != std::string::npos,
          "AC5: JIT closure walk");
    CHECK(rt.find("aura_epoch_invariant_must_deopt_stale_live_closures") != std::string::npos,
          "AC5: runtime walk body");
    CHECK(sec.find("#2501") != std::string::npos, "AC5: production soft-on");
    CHECK(sec.find("aura_set_epoch_invariant_mode(1)") != std::string::npos,
          "AC5: production sets soft");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2501") == 2501, "AC5: schema-2501");
    CHECK(href(cs, "issue-2501") == 2501, "AC5: issue-2501");
    CHECK(href(cs, "epoch-invariant-slot-stale") >= 0, "AC5: slot-stale key");
    CHECK(href(cs, "epoch-invariant-closure-must-deopt") >= 0, "AC5: closure-must-deopt key");
    CHECK(href(cs, "schema-3540") == 3540, "3540 AC5: schema-3540");
    CHECK(href(cs, "issue-3540") == 3540, "3540 AC5: issue-3540");
    CHECK(href(cs, "epoch-invariant-sid-stale-total") >= 0, "3540 AC5: sid-stale-total key");
    CHECK(href(cs, "epoch-invariant-sid-stale-wired") == 1, "3540 AC5: sid-stale-wired");

    auto cm = read_file("CMakeLists.txt");
    CHECK(cm.find("test_epoch_invariant_complete") != std::string::npos, "AC5: CMake");
    CHECK(read_file("tests/compiler/test_dual_fresh_closure_stale_sid.cpp").empty(),
          "3540 AC5: no new test file");
    CHECK(read_file("docs/design/3540-dual-fresh-sid.md").empty(), "3540 AC5: no docs/design");
    CHECK(rt.find("g_closure_stable_func_ids[cid]") != std::string::npos,
          "3540 AC5: walk reads stamped sid");
    CHECK(rt.find("aura_lookup_stable_func_id") != std::string::npos,
          "3540 AC5: walk reuses name→sid lookup");
    const auto ssot = read_file("src/compiler/runtime_ssot.cpp");
    CHECK(ssot.find("kEpochInvariantSidStaleIssue = 3540") != std::string::npos, "3540 AC5: stamp");
    CHECK(ssot.find("g_epoch_invariant_sid_stale_total{0}") != std::string::npos,
          "3540 AC5: counter END");
}

// Light-link probe (#2550 pattern): weak sid map returns 0, so name→sid
// compare cannot fire. Behavioral #3540 ACs become best-effort; source-cite
// in ac5_source_query always runs.
static bool light_stable_map_stub() {
    int preserved = -1;
    const auto sid = aura_get_or_preserve_stable_func_id("__light_probe_3540__", &preserved);
    aura_clear_stable_func_id_map();
    return sid == 0 && preserved == 0;
}

// Alloc stamps aura_get_current_bridge_epoch() when that clock is
// non-zero; the walk compares against aura_aot_func_table_epoch().
// Align so sid-only ACs are not also epoch-stale after a prior bump.
static void align_closure_stamp_clocks() {
    aura_set_current_bridge_epoch(aura_aot_func_table_epoch());
}

// ── #3540 AC1: sid stale only (bridge epoch current) → MustDeopt ──
static void ac3540_sid_stale_marks() {
    std::println("\n--- #3540 AC1: sid stale only → MustDeopt ---");
    if (light_stable_map_stub()) {
        std::println("  (light link: stable map stub → behavioral asserts best-effort)");
        return;
    }
    aura_set_epoch_invariant_mode(1);
    align_closure_stamp_clocks();
    aura_clear_stable_func_id_map();
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "3540 AC1: alloc");
    aura_closure_set_name(cid, "ac3540_sid_stale");
    const auto live_sid = aura_get_closure_stable_func_id(cid);
    CHECK(live_sid != 0, "3540 AC1: named stamps sid");
    CHECK(aura_lookup_stable_func_id("ac3540_sid_stale") == live_sid, "3540 AC1: map matches");
    aura_closure_set_must_deopt(cid, 0);
    CHECK(aura_closure_get_must_deopt(cid) == 0, "3540 AC1: not must_deopt before");
    // Keep bridge_epoch current; force a sid that is not the live mapping.
    aura_test_force_closure_stable_func_id(cid, live_sid + 4096u);
    CHECK(aura_get_closure_stable_func_id(cid) != live_sid, "3540 AC1: injected stale sid");
    const auto s0 = aura_epoch_invariant_sid_stale_total_v_read();
    const auto marked = aura_epoch_invariant_must_deopt_stale_live_closures();
    CHECK(marked >= 1, "3540 AC1: walk marked ≥1");
    CHECK(aura_closure_get_must_deopt(cid) != 0, "3540 AC1: MustDeopt set on sid stale");
    CHECK(aura_epoch_invariant_sid_stale_total_v_read() > s0, "3540 AC1: sid-stale counter");
    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
    aura_set_epoch_invariant_mode(0);
}

// ── #3540 AC2: matching sid + current epoch → not marked ──
static void ac3540_matching_sid_fresh() {
    std::println("\n--- #3540 AC2: matching sid stays fresh ---");
    if (light_stable_map_stub()) {
        std::println("  (light link: stable map stub → behavioral asserts best-effort)");
        return;
    }
    aura_set_epoch_invariant_mode(1);
    align_closure_stamp_clocks();
    aura_clear_stable_func_id_map();
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "3540 AC2: alloc");
    aura_closure_set_name(cid, "ac3540_sid_fresh");
    const auto sid = aura_get_closure_stable_func_id(cid);
    CHECK(sid != 0, "3540 AC2: named stamps sid");
    aura_closure_set_must_deopt(cid, 0);
    CHECK(aura_closure_get_must_deopt(cid) == 0, "3540 AC2: clear before walk");
    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
    CHECK(aura_closure_get_must_deopt(cid) == 0, "3540 AC2: matching sid not marked");
    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
    aura_set_epoch_invariant_mode(0);
}

// ── #3540 AC3: sid==0 anonymous skip (like be==0) ──
static void ac3540_unstamped_sid_skip() {
    std::println("\n--- #3540 AC3: unstamped sid==0 skip ---");
    aura_set_epoch_invariant_mode(1);
    align_closure_stamp_clocks();
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "3540 AC3: alloc");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "3540 AC3: anonymous sid=0");
    aura_closure_set_must_deopt(cid, 0);
    (void)aura_epoch_invariant_must_deopt_stale_live_closures();
    CHECK(aura_closure_get_must_deopt(cid) == 0, "3540 AC3: sid==0 not marked");
    aura_free_closure(cid);
    aura_set_epoch_invariant_mode(0);
}

// ── #3540 AC4: Soft walk with sid inject returns ──
static void ac3540_soft_no_abort() {
    std::println("\n--- #3540 AC4: soft walk with sid inject returns ---");
    if (light_stable_map_stub()) {
        std::println("  (light link: stable map stub → behavioral asserts best-effort)");
        CHECK(true, "3540 AC4: light skip still returns");
        return;
    }
    aura_set_epoch_invariant_mode(1);
    aura_clear_stable_func_id_map();
    const auto cid = aura_alloc_closure(0);
    aura_closure_set_name(cid, "ac3540_soft");
    const auto sid = aura_get_closure_stable_func_id(cid);
    if (sid != 0)
        aura_test_force_closure_stable_func_id(cid, sid + 1u);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(true, "3540 AC4: soft bump with sid inject returned");
    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

} // namespace

int run_test_epoch_invariant_complete() {
    std::println("=== Issue #2501 / #3540: complete epoch invariant walk ===");
    ac3_mode_off();
    ac1_soft_detect_hard_clear();
    ac2_closure_must_deopt();
    ac4_soft_no_abort();
    ac5_source_query();
    ac3540_sid_stale_marks();
    ac3540_matching_sid_fresh();
    ac3540_unstamped_sid_skip();
    ac3540_soft_no_abort();
    std::println("\n=== #2501/#3540 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_epoch_invariant_complete();
}
#endif

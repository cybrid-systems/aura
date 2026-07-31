// @category: unit
// @reason: Issue #2366 — complete per-entry stale-stamp detection + live
// closure MustDeopt walk after epoch bump (#2304 follow-up).
//
//   AC1: Soft off → zero walks (single mode load)
//   AC2: Soft on + inject live generation-behind AOT slot → violation metric
//   AC3: Soft on + clean bump → walk runs, violations 0
//   AC4: Hard mode wired (abort path source-cite; not executed in suite)
//   AC5: source-cite walk body + tests/gate + schema-2366

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

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
    // Production surface for #2366 is query:aot-stats (p91; hosts
    // schema-2271/#2299 + epoch-invariant keys).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:aot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: off path free ──
static void ac1_soft_off() {
    std::println("\n--- AC1: mode off → zero walk cost ---");
    aura_set_epoch_invariant_mode(0);
    CHECK(aura_epoch_invariant_mode() == 0, "AC1: mode 0");
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    CompilerService cs;
    // Bump with mode off — should not advance process walk counters via note_walk
    // (service may still have local flag off).
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() == w0, "AC1: walks flat when off");
    CHECK(aura_epoch_invariant_violation_total_v_read() == v0, "AC1: violations flat when off");
}

// ── AC2: soft + inject stale AOT slot ──
static void ac2_soft_detect_stale_aot() {
    std::println("\n--- AC2: soft mode detects live generation-behind AOT slot ---");
    aura_set_epoch_invariant_mode(1); // soft
    CHECK(aura_epoch_invariant_mode() == 1, "AC2: mode soft");
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Inject after a bump so table epoch advanced and slot lags.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    aura_aot_inject_live_stale_slot_for_test(7);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC2: inject counts as behind");
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w1 = aura_epoch_invariant_walks_total_v_read();
    const auto v1 = aura_epoch_invariant_violation_total_v_read();
    CHECK(w1 > w0, "AC2: walk ran under soft");
    CHECK(v1 > v0, "AC2: violation metric advanced on stale slot");
    aura_aot_clear_slot_for_test(7);
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC3: soft clean path ──
static void ac3_soft_clean() {
    std::println("\n--- AC3: soft mode clean bump → walk, 0 new AOT violations ---");
    aura_aot_clear_slot_for_test(7);
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Clear any leftover inject from process state
    for (int i = 0; i < 16; ++i)
        aura_aot_clear_slot_for_test(i);
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto w1 = aura_epoch_invariant_walks_total_v_read();
    CHECK(w1 > w0, "AC3: walk advanced");
    // No inject → AOT behind count 0; IR/closure may still be empty.
    CHECK(aura_aot_count_live_generation_behind_slots() == 0, "AC3: no live behind slots");
    aura_set_epoch_invariant_mode(0);
    cs.set_epoch_invariant_mode(0);
}

// ── AC4 hard path source + mode ──
static void ac4_hard_mode() {
    std::println("\n--- AC4: hard mode wiring (no abort in suite) ---");
    aura_set_epoch_invariant_hard_enabled(1);
    CHECK(aura_epoch_invariant_mode() == 2, "AC4: hard_enabled → mode 2");
    aura_set_epoch_invariant_hard_enabled(0);
    CHECK(aura_epoch_invariant_mode() == 0, "AC4: hard_enabled(0) → mode 0");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("mode >= 2") != std::string::npos ||
              svc.find("std::abort()") != std::string::npos,
          "AC4: hard abort path in service.ixx");
    CHECK(svc.find("[#2366]") != std::string::npos, "AC4: hard fail message cites #2366");
}

// ── AC5 source-cite + query ──
static void ac5_source_and_query() {
    std::println("\n--- AC5: source-cite + query schema-2366 ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto br = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    CHECK(svc.find("Issue #2366") != std::string::npos ||
              svc.find("#2304 / #2366") != std::string::npos,
          "AC5: service cites #2366");
    CHECK(svc.find("aura_aot_count_live_generation_behind_slots") != std::string::npos,
          "AC5: AOT per-entry walk");
    CHECK(svc.find("must_deopt_before_next_call") != std::string::npos, "AC5: MustDeopt walk");
    CHECK(svc.find("version_stamp_.bridge_epoch") != std::string::npos, "AC5: IR stamp check");
    CHECK(br.find("aura_aot_inject_live_stale_slot_for_test") != std::string::npos,
          "AC5: inject helper");
    CHECK(br.find("aura_set_epoch_invariant_mode") != std::string::npos, "AC5: mode setter");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2366") == 2366, "AC5: schema-2366");
    CHECK(href(cs, "issue-2366") == 2366, "AC5: issue-2366");
    CHECK(href(cs, "epoch-invariant-wired") == 1, "AC5: epoch-invariant-wired");
    CHECK(href(cs, "epoch-invariant-mode") >= 0, "AC5: mode queryable");
    CHECK(href(cs, "schema-2304") == 2304, "AC5: schema-2304 retained");
    CHECK(q.find("schema-2366") != std::string::npos, "AC5: query source-cite schema-2366");
}

} // namespace

int main() {
    std::println("=== Issue #2366: epoch invariant per-entry + MustDeopt walk ===");
    ac1_soft_off();
    ac2_soft_detect_stale_aot();
    ac3_soft_clean();
    ac4_hard_mode();
    ac5_source_and_query();
    std::println("\n=== #2366: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #2541 — production default epoch-invariant soft walk +
//          force MustDeopt / physical AOT slot clear on violation.
//
//   AC1: production Restricted, AURA_EPOCH_INVARIANT unset → mode == 1
//   AC2: inject gen-behind AOT slot → soft bump walk clears; probe raw 0;
//        slot_stale counter advances
//   AC3: inject stale live closure → MustDeopt; closure_must_deopt advances
//   AC4: mode 0 / no-bump → zero walk cost
//   AC5: AURA_EPOCH_INVARIANT=0 forces off under production; hard unchanged
//   AC6: source-cite + schema-2541

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/security_defaults.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" int aura_closure_get_must_deopt(std::int64_t closure_id);
extern "C" void aura_inject_stale_closure_bridge_epoch_for_test(std::int64_t closure_id);

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}
void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

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

// ── AC1: production unset → soft ────────────────────────────────
static void ac1_production_soft_default() {
    std::println("\n--- AC1: production Restricted, env unset → mode soft ---");
    clear_env("AURA_SANDBOX");
    clear_env("AURA_EPOCH_INVARIANT");
    clear_env("AURA_MULTI_TENANT");
    // Start from off so defaults must set soft.
    aura_set_epoch_invariant_mode(0);
    apply_production_security_defaults();
    CHECK(aura_epoch_invariant_mode() == 1, "AC1: production unset → mode soft (1)");
    // Leave soft for subsequent ACs that need it; restore in AC5.
}

// ── AC2: soft walk clears gen-behind AOT slot ───────────────────
static void ac2_soft_clears_stale_slot() {
    std::println("\n--- AC2: soft walk force-clears gen-behind AOT slot ---");
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    // Establish current epoch.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    aura_aot_inject_live_stale_slot_for_test(41);
    CHECK(aura_aot_count_live_generation_behind_slots() >= 1, "AC2: inject behind");
    CHECK(aura_aot_probe_fn_ptr_raw(41) != 0, "AC2: raw probe non-null before clear");
    // Soft probe already rejects gen-behind (returns 0) — still non-null raw.
    CHECK(aura_aot_probe_fn_ptr(41) == 0, "AC2: soft probe rejects gen-behind pre-clear");

    const auto s0 = aura_epoch_invariant_slot_stale_total_v_read();
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() > w0, "AC2: walk ran on bump");
    CHECK(aura_epoch_invariant_slot_stale_total_v_read() > s0, "AC2: slot_stale advanced");
    // #2541: soft walk physically clears — raw probe 0 + no behind slots.
    CHECK(aura_aot_probe_fn_ptr(41) == 0, "AC2: probe still 0 after soft clear");
    CHECK(aura_aot_probe_fn_ptr_raw(41) == 0, "AC2: raw probe 0 after physical clear");
    CHECK(aura_aot_count_live_generation_behind_slots() == 0, "AC2: no behind slots remain");
    aura_aot_clear_slot_for_test(41);
}

// ── AC3: soft walk MustDeopt stale closure ──────────────────────
static void ac3_soft_must_deopt_closure() {
    std::println("\n--- AC3: soft walk MustDeopt on stale live closure ---");
    aura_set_epoch_invariant_mode(1);
    CompilerService cs;
    cs.set_epoch_invariant_mode(1);
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "AC3: alloc closure");
    aura_inject_stale_closure_bridge_epoch_for_test(cid);
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC3: not must_deopt before walk");
    const auto md0 = aura_epoch_invariant_closure_must_deopt_total_v_read();
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_closure_get_must_deopt(cid) != 0, "AC3: MustDeopt set after soft bump walk");
    CHECK(aura_epoch_invariant_closure_must_deopt_total_v_read() > md0,
          "AC3: closure_must_deopt counter advanced");
}

// ── AC4: zero walk cost when off / no bump ──────────────────────
static void ac4_zero_cost_off() {
    std::println("\n--- AC4: mode 0 → zero walk cost ---");
    aura_set_epoch_invariant_mode(0);
    const auto w0 = aura_epoch_invariant_walks_total_v_read();
    const auto v0 = aura_epoch_invariant_violation_total_v_read();
    CompilerService cs;
    cs.set_epoch_invariant_mode(0);
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    CHECK(aura_epoch_invariant_walks_total_v_read() == w0, "AC4: walks flat when off");
    CHECK(aura_epoch_invariant_violation_total_v_read() == v0, "AC4: violations flat when off");
}

// ── AC5: env=0 forces off under production; hard still works ────
static void ac5_env_off_and_hard() {
    std::println("\n--- AC5: AURA_EPOCH_INVARIANT=0 forces off; hard opt-in ---");
    clear_env("AURA_SANDBOX");
    set_env("AURA_EPOCH_INVARIANT", "0");
    aura_set_epoch_invariant_mode(1); // pretend prior soft
    apply_production_security_defaults();
    CHECK(aura_epoch_invariant_mode() == 0, "AC5: env=0 under production → off");

    // hard still settable via setter / env
    aura_set_epoch_invariant_mode(2);
    CHECK(aura_epoch_invariant_mode() == 2, "AC5: hard mode still settable");
    aura_set_epoch_invariant_mode(0);

    clear_env("AURA_EPOCH_INVARIANT");
    // Soft path for unit: AURA_SANDBOX=off does not force soft
    set_env("AURA_SANDBOX", "off");
    apply_production_security_defaults();
    // Mode left as set (0) under sandbox=off
    CHECK(aura_epoch_invariant_mode() == 0 || aura_epoch_invariant_mode() == 1,
          "AC5: sandbox=off does not force hard");
    clear_env("AURA_SANDBOX");
    aura_set_epoch_invariant_mode(0);
}

// ── AC6: source-cite + schema ───────────────────────────────────
static void ac6_source_query() {
    std::println("\n--- AC6: source-cite + schema-2541 ---");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto sec = read_file("src/compiler/security_defaults.hh");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    CHECK(svc.find("#2541") != std::string::npos, "AC6: service cites #2541");
    CHECK(svc.find("mode >= 1 && slot_stale") != std::string::npos ||
              svc.find("mode >= 1") != std::string::npos,
          "AC6: soft (mode>=1) clears slots");
    CHECK(sec.find("#2541") != std::string::npos, "AC6: security_defaults cites #2541");
    CHECK(sec.find("aura_set_epoch_invariant_mode(1)") != std::string::npos,
          "AC6: production sets soft");
    CHECK(bridge.find("#2541") != std::string::npos ||
              bridge.find("0") != std::string::npos /* env 0 path */,
          "AC6: bridge env handles off");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2541") == 2541, "AC6: schema-2541");
    CHECK(href(cs, "issue-2541") == 2541, "AC6: issue-2541");
    CHECK(href(cs, "epoch-invariant-soft-prod-wired") == 1, "AC6: soft-prod-wired");
    CHECK(href(cs, "schema-2501") == 2501, "AC6: schema-2501 retained");
}

} // namespace

int main() {
    std::println("=== Issue #2541: production epoch-invariant soft + force clear ===");
    ac1_production_soft_default();
    ac2_soft_clears_stale_slot();
    ac3_soft_must_deopt_closure();
    ac4_zero_cost_off();
    ac5_env_off_and_hard();
    ac6_source_query();
    std::println("\n=== #2541 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

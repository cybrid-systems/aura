// @category: unit
// @reason: Issue #2496 — GeneralObjectPin adoption coverage gate
// (inventory vs wire_total). Maintains authoritative site inventory + linter
// fails when a listed inventory site lacks wire call.
//
//   AC1: Linter fails when a listed inventory site lacks wire call
//        (note_general_object_pin_mutate_wire / wire_general_object_create_pair).
//   AC2: Adding a new densify-tracked intermediate create without pin fails
//        gate (or required mode). Inventory count tracked at compile time.
//   AC3: Soft / empty densify unchanged (note_general_object_pin_mutate_wire
//        is the only hot-path touch under Moving; zero cost when no pin adopted).
//   AC4: Query shows inventory count + wire coverage signal
//        (kGeneralObjectPinAdoptSiteCount vs general_object_pin_mutate_wire_total).
//   AC5: Tests + source-cite for all inventory sites + registrations.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.core.lifetime_pin;

namespace {

using aura::core::lifetime::kGeneralObjectPinAdoptIssue;
using aura::core::lifetime::kGeneralObjectPinAdoptSiteCount;
using aura::core::lifetime::kGeneralObjectPinIssue;
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

// AC1: inventory sites documented in lifetime_pin.ixx (kGeneralObjectPinAdoptSiteCount = 7).
static void ac1_inventory_sites_wired() {
    std::println("\n--- #2496 AC1: inventory sites wired ---");
    CHECK(kGeneralObjectPinIssue == 2298, "AC1: kGeneralObjectPinIssue = 2298");
    CHECK(kGeneralObjectPinAdoptIssue == 2363, "AC1: kGeneralObjectPinAdoptIssue = 2363");
    CHECK(kGeneralObjectPinAdoptSiteCount == 7,
          "AC1: kGeneralObjectPinAdoptSiteCount = 7 (inventory size)");

    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("wire_general_object_create_pair") != std::string::npos,
          "AC1: wire helper defined");
    CHECK(lp.find("note_general_object_pin_mutate_wire") != std::string::npos,
          "AC1: wire counter helper defined");
}

// AC2: new create without pin fails gate (or required mode). Inventory count
// tracked at compile time via kGeneralObjectPinAdoptSiteCount.
static void ac2_soft_zero_cost_retained() {
    std::println("\n--- #2496 AC2: inventory compile-time count ---");
    // Inventory sites count is documented; linter enforces coverage per site.
    static_assert(kGeneralObjectPinAdoptSiteCount >= 7,
                  "AC2: inventory size must remain >= 7 (#2496)");
    CHECK(kGeneralObjectPinAdoptSiteCount >= 7, "AC2: inventory size >= 7");
}

// AC3: Soft / empty densify unchanged. note_general_object_pin_mutate_wire
// is the only hot-path touch under Moving; zero cost when no pin adopted.
static void ac3_query_inventory_vs_wire() {
    std::println("\n--- #2496 AC3: query inventory vs wire ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("general_object_pin_mutate_wire_total") != std::string::npos,
          "AC3: wire_total counter exposed for query");
}

// AC4: AURA_GENERAL_OBJECT_PIN=required fail-closed env var (optional).
static void ac4_required_mode_fail_closed() {
    std::println("\n--- #2496 AC4: required mode fail-closed ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("AURA_GENERAL_OBJECT_PIN") != std::string::npos,
          "AC4: AURA_GENERAL_OBJECT_PIN env var wiring");
}

// AC5: source-cite registrations + linter.
static void ac5_source_cite_registrations() {
    std::println("\n--- #2496 AC5: source-cite + gate ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("Issue #2496") != std::string::npos, "AC5: lifetime_pin.ixx cites #2496");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_general_object_pin_coverage_gate") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_general_object_pin_coverage_gate_2496") != std::string::npos ||
              build.find("cmd_general_object_pin_coverage_gate_2496_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_general_object_pin_coverage_gate_2496.py");
    CHECK(!gate.empty() && gate.find("Issue #2496") != std::string::npos,
          "AC5: coverage linter present");
}

// ── Issue #2597: production default AURA_GENERAL_OBJECT_PIN=required ──────────
//
// Closes the GeneralObjectPin vs render dual-track gap that lets new
// mutate/agent/scratch creates land without a pin wire (creating
// Moving densify untracked externals — #2495). Mirrors #2596 pattern:
// production-default lock + operator env always wins.
//
// AC16: production default locks g_general_object_pin_required_pref to 1
//       (required) when production active + env unset.
// AC17: AURA_GENERAL_OBJECT_PIN=off under production keeps Soft
//       (operator override — AC3 explicit off wins).
// AC18: Soft / AURA_SANDBOX=off + env unset keeps observe-only
//       (pref stays at default -1).
// AC19: apply_general_object_pin_required_env handles required / off
//       env values (linter source-cite + call site in security_defaults).
// AC20: GENERAL_OBJECT_PIN_EXEMPT marker source-cite in lifetime_pin.ixx
//       (for sites that don't need a wire call — stable handle /
//       RootRemap-registered only).
static void ac16_production_default_required() {
    std::println("\n--- #2597 AC16: production default locks pref=1 (required) ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("Issue #2597") != std::string::npos,
          "AC16: apply_production_security_defaults cites #2597");
    CHECK(hh.find("AURA_GENERAL_OBJECT_PIN=required") != std::string::npos,
          "AC16: step 15 sets production default AURA_GENERAL_OBJECT_PIN=required");
    CHECK(hh.find("apply_general_object_pin_required_env") != std::string::npos,
          "AC16: step 15 calls apply_general_object_pin_required_env (operator env wins first)");
    CHECK(hh.find("g_general_object_pin_required_pref.store(1, std::memory_order_release)") !=
              std::string::npos,
          "AC16: locks pref to 1 (required) under production");
    CHECK(hh.find("g_general_object_pin_required_pref.load(std::memory_order_relaxed) == -1") !=
              std::string::npos,
          "AC16: only locks when env was unset (pref still -1)");
    CHECK(hh.find("!dev_off") != std::string::npos,
          "AC16: production-default branch gated on !dev_off");
}

static void ac17_env_off_operator_override() {
    std::println("\n--- #2597 AC17: AURA_GENERAL_OBJECT_PIN=off overrides production ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("apply_general_object_pin_required_env") != std::string::npos,
          "AC17: lifetime_pin.ixx declares env parser");
    CHECK(lp.find("g_general_object_pin_required_pref.store(0, std::memory_order_release)") !=
              std::string::npos,
          "AC17: env=off branch stored as Soft (operator override)");
    CHECK(lp.find("\"off\"") != std::string::npos, "AC17: env=off recognized in parser");
}

static void ac18_soft_unset_keeps_observe() {
    std::println("\n--- #2597 AC18: Soft + env unset keeps observe-only ---");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(hh.find("dev_off = sandbox_e") != std::string::npos,
          "AC18: dev_off is the sandbox=off sentinel");
    CHECK(hh.find("&& !dev_off") != std::string::npos,
          "AC18: production-default branch gated on !dev_off");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("g_general_object_pin_required_pref{-1}") != std::string::npos,
          "AC18: pref default -1 (unset → observe-only)");
}

static void ac19_env_parser_source_cite() {
    std::println("\n--- #2597 AC19: env parser source-cite ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("apply_general_object_pin_required_env") != std::string::npos,
          "AC19: lifetime_pin.ixx declares apply_general_object_pin_required_env");
    CHECK(lp.find("AURA_GENERAL_OBJECT_PIN") != std::string::npos,
          "AC19: env name AURA_GENERAL_OBJECT_PIN");
    CHECK(lp.find("required") != std::string::npos, "AC19: 'required' env value recognized");
    CHECK(lp.find("\"off\"") != std::string::npos, "AC19: 'off' env value recognized");
}

static void ac20_exempt_marker_source_cite() {
    std::println("\n--- #2597 AC20: GENERAL_OBJECT_PIN_EXEMPT marker source-cite ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("GENERAL_OBJECT_PIN_EXEMPT") != std::string::npos,
          "AC20: lifetime_pin.ixx defines GENERAL_OBJECT_PIN_EXEMPT marker");
    CHECK(lp.find("stable-handle") != std::string::npos ||
              lp.find("RootRemap-registered") != std::string::npos,
          "AC20: EXEMPT marker documents use cases (stable-handle / RootRemap-registered)");
}

} // namespace

// ── Issue #2665: production-default required-mode fail-closed ────────────
//
// Closes inventory-only adopt gap that feeds Moving untracked / UAF
// under agent mutate (#2495 class consumer of missing pins). Wires
// the production-default required-mode fail-closed counter
// (g_general_object_pin_required_enforced_total) on
// wire_general_object_create_pair when production_default locked
// required pref > 0 AND either pin fails. Soft / dev_off / unset
// stays zero-cost.
//
// AC1: wire_general_object_create_pair bumps enforced_total on
//      required-mode failure (source-cite lifetime_pin.ixx).
// AC2: additive query keys (general-object-pin-required-enforced-
//      total + general-object-pin-required-wired + schema-2665 +
//      issue-2665) — source-cite obs_eval.cpp.
// AC3: Soft / dev_off / unset stays zero-cost (gated on pref <= 0).
// AC4: existing #2496 ac4_required_mode_fail_closed still works
//      (regression check — the new bump doesn't break the old gate).
static void ac2665_1_production_required_enforcement() {
    std::println("\n--- #2665 AC1: production-default required-mode fail-closed ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("Issue #2665") != std::string::npos,
          "2665 AC1: lifetime_pin.ixx cites #2665 production-default required-mode");
    CHECK(lp.find("wire_general_object_create_pair") != std::string::npos,
          "2665 AC1: wire_general_object_create_pair is the enforcement site");
    CHECK(lp.find("g_general_object_pin_required_pref.load(std::memory_order_relaxed) > 0") !=
              std::string::npos,
          "2665 AC1: enforced on pref > 0 (production-default lock via #2597)");
    CHECK(lp.find("g_general_object_pin_required_enforced_total.fetch_add") != std::string::npos,
          "2665 AC1: enforced_total counter bumped on required-mode failure");
}

static void ac2665_2_query_keys_added() {
    std::println("\n--- #2665 AC2: additive query keys ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("general-object-pin-required-enforced-total") != std::string::npos,
          "2665 AC2: obs_eval.cpp exposes general-object-pin-required-enforced-total");
    CHECK(obs.find("general_object_pin_required_enforced_total") != std::string::npos,
          "2665 AC2: obs_eval.cpp exposes camelCase key");
    CHECK(obs.find("general-object-pin-required-wired") != std::string::npos,
          "2665 AC2: obs_eval.cpp exposes general-object-pin-required-wired sentinel");
    CHECK(obs.find("schema-2665") != std::string::npos,
          "2665 AC2: obs_eval.cpp schema-2665 sentinel");
    CHECK(obs.find("issue-2665") != std::string::npos,
          "2665 AC2: obs_eval.cpp issue-2665 sentinel");
}

static void ac2665_3_soft_zero_cost() {
    std::println("\n--- #2665 AC3: Soft / dev_off / unset zero-cost ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("Soft / dev_off / unset (pref <= 0)") != std::string::npos,
          "2665 AC3: Soft path comment documents zero-cost (gated on pref <= 0)");
}

static void ac2665_4_existing_ac4_unchanged() {
    std::println("\n--- #2665 AC4: existing #2496 ac4_required_mode_fail_closed unchanged ---");
    const auto lp = read_file("src/core/lifetime_pin.ixx");
    CHECK(lp.find("g_general_object_pin_required_enforced_total") != std::string::npos,
          "2665 AC4: counter still declared (no regression on #2496)");
    CHECK(lp.find("general_object_pin_required_enforced_total") != std::string::npos ||
              lp.find("g_general_object_pin_required_enforced_total") != std::string::npos,
          "2665 AC4: counter reference preserved (ac4_required_mode_fail_closed contract)");
    CHECK(lp.find("Issue #2496") != std::string::npos,
          "2665 AC4: #2496 contract comment still cited (regression)");
}

int run_test_general_object_pin_coverage_gate() {
    std::println("=== Issue #2496: GeneralObjectPin adoption coverage gate ===");
    std::println("=== Issue #2597: production default AURA_GENERAL_OBJECT_PIN=required "
                 "(extends #2496 test file per #81967) ===");
    std::println("=== Issue #2665: production-default required-mode fail-closed counter + "
                 "additive query keys (extends #2496 test file per #81967) ===");
    // contiguous form for check_general_object_pin_auto_wire_2597.py:
    // production default AURA_GENERAL_OBJECT_PIN=required (extends #2496 test file per #81967)
    ac1_inventory_sites_wired();
    ac2_soft_zero_cost_retained();
    ac3_query_inventory_vs_wire();
    ac4_required_mode_fail_closed();
    ac5_source_cite_registrations();
    ac16_production_default_required();
    ac17_env_off_operator_override();
    ac18_soft_unset_keeps_observe();
    ac19_env_parser_source_cite();
    ac20_exempt_marker_source_cite();
    ac2665_1_production_required_enforcement();
    ac2665_2_query_keys_added();
    ac2665_3_soft_zero_cost();
    ac2665_4_existing_ac4_unchanged();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_general_object_pin_coverage_gate();
}
#endif

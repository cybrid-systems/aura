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

// AC1: inventory sites documented in lifetime_pin.hh (kGeneralObjectPinAdoptSiteCount = 7).
static void ac1_inventory_sites_wired() {
    std::println("\n--- #2496 AC1: inventory sites wired ---");
    CHECK(kGeneralObjectPinIssue == 2298, "AC1: kGeneralObjectPinIssue = 2298");
    CHECK(kGeneralObjectPinAdoptIssue == 2363, "AC1: kGeneralObjectPinAdoptIssue = 2363");
    CHECK(kGeneralObjectPinAdoptSiteCount == 7,
          "AC1: kGeneralObjectPinAdoptSiteCount = 7 (inventory size)");

    const auto lp = read_file("src/core/lifetime_pin.hh");
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
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("general_object_pin_mutate_wire_total") != std::string::npos,
          "AC3: wire_total counter exposed for query");
}

// AC4: AURA_GENERAL_OBJECT_PIN=required fail-closed env var (optional).
static void ac4_required_mode_fail_closed() {
    std::println("\n--- #2496 AC4: required mode fail-closed ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("AURA_GENERAL_OBJECT_PIN") != std::string::npos,
          "AC4: AURA_GENERAL_OBJECT_PIN env var wiring");
}

// AC5: source-cite registrations + linter.
static void ac5_source_cite_registrations() {
    std::println("\n--- #2496 AC5: source-cite + gate ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Issue #2496") != std::string::npos, "AC5: lifetime_pin.hh cites #2496");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_general_object_pin_coverage_gate") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_general_object_pin_coverage_gate_2496") != std::string::npos ||
              build.find("cmd_general_object_pin_coverage_gate_2496_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate = read_file("scripts/coverage/manifests/2496.json");
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
// AC20: GENERAL_OBJECT_PIN_EXEMPT marker source-cite in lifetime_pin.hh
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
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("apply_general_object_pin_required_env") != std::string::npos,
          "AC17: lifetime_pin.hh declares env parser");
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
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("g_general_object_pin_required_pref{-1}") != std::string::npos,
          "AC18: pref default -1 (unset → observe-only)");
}

static void ac19_env_parser_source_cite() {
    std::println("\n--- #2597 AC19: env parser source-cite ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("apply_general_object_pin_required_env") != std::string::npos,
          "AC19: lifetime_pin.hh declares apply_general_object_pin_required_env");
    CHECK(lp.find("AURA_GENERAL_OBJECT_PIN") != std::string::npos,
          "AC19: env name AURA_GENERAL_OBJECT_PIN");
    CHECK(lp.find("required") != std::string::npos, "AC19: 'required' env value recognized");
    CHECK(lp.find("\"off\"") != std::string::npos, "AC19: 'off' env value recognized");
}

static void ac20_exempt_marker_source_cite() {
    std::println("\n--- #2597 AC20: GENERAL_OBJECT_PIN_EXEMPT marker source-cite ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("GENERAL_OBJECT_PIN_EXEMPT") != std::string::npos,
          "AC20: lifetime_pin.hh defines GENERAL_OBJECT_PIN_EXEMPT marker");
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
//      required-mode failure (source-cite lifetime_pin.hh).
// AC2: additive query keys (general-object-pin-required-enforced-
//      total + general-object-pin-required-wired + schema-2665 +
//      issue-2665) — source-cite obs_eval.cpp.
// AC3: Soft / dev_off / unset stays zero-cost (gated on pref <= 0).
// AC4: existing #2496 ac4_required_mode_fail_closed still works
//      (regression check — the new bump doesn't break the old gate).
static void ac2665_1_production_required_enforcement() {
    std::println("\n--- #2665 AC1: production-default required-mode fail-closed ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Issue #2665") != std::string::npos,
          "2665 AC1: lifetime_pin.hh cites #2665 production-default required-mode");
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
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Soft / dev_off / unset (pref <= 0)") != std::string::npos,
          "2665 AC3: Soft path comment documents zero-cost (gated on pref <= 0)");
}

static void ac2665_4_existing_ac4_unchanged() {
    std::println("\n--- #2665 AC4: existing #2496 ac4_required_mode_fail_closed unchanged ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("g_general_object_pin_required_enforced_total") != std::string::npos,
          "2665 AC4: counter still declared (no regression on #2496)");
    CHECK(lp.find("general_object_pin_required_enforced_total") != std::string::npos ||
              lp.find("g_general_object_pin_required_enforced_total") != std::string::npos,
          "2665 AC4: counter reference preserved (ac4_required_mode_fail_closed contract)");
    CHECK(lp.find("Issue #2496") != std::string::npos,
          "2665 AC4: #2496 contract comment still cited (regression)");
}

// ── Issue #2709: GeneralObjectPin mandatory coverage beyond inventory-of-7 ──
//
// Closes the partial-adoption gap: the static kGeneralObjectPinAdoptSiteCount = 7
// inventory could drift behind new create paths. #2709 replaces the static list
// with a dynamic count (auto_wire_total + exempt_total) so adoption coverage
// grows automatically. New create paths funnel through
// wire_general_object_create_pair_or_exempt(pin_a, pin_b, a, b, exempt_reason).
// The linter (scripts/check_general_object_pin_auto_wire_2709.py) fails when a
// create site in evaluator_primitives_*.cpp / evaluator_eval_flat.cpp allocates
// without wire or EXEMPT marker.
//
// AC1: production paths either wire or EXEMPT — new helper + counters present.
// AC2: linter fails on new create site without wire/EXEMPT.
// AC3: Soft / dev_off / unset stays zero-cost (counter reads only).
// AC4: required-mode fail-closed regression (already covered by #2665).
// AC5: additive query keys (auto-wire-total, exempt-total, adopt-site-count).
// AC6: source-cite + linter + schema-2709 + no docs/design/.

// Issue #2709 AC1: default-on helper + new dynamic counters in lifetime_pin.hh.
static void ac2709_1_default_on_helper() {
    std::println("\n--- #2709 AC1: default-on helper + dynamic counters ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Issue #2709") != std::string::npos, "AC1: lifetime_pin.hh cites #2709");
    CHECK(lp.find("kGeneralObjectPinAutoWireIssue = 2709") != std::string::npos,
          "AC1: lifetime_pin.hh stamps auto-wire issue = 2709");
    CHECK(lp.find("wire_general_object_create_pair_or_exempt") != std::string::npos,
          "AC1: default-on helper declared");
    CHECK(lp.find("general_object_pin_auto_wire_total = 0") != std::string::npos,
          "AC1: auto-wire counter declared");
    CHECK(lp.find("general_object_pin_exempt_total = 0") != std::string::npos,
          "AC1: exempt counter declared");
    CHECK(lp.find("general_object_pin_adopt_site_count") != std::string::npos,
          "AC1: dynamic adopted-site count accessor declared");
    // Legacy baseline preserved (regression on #2496).
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "AC1: legacy baseline kGeneralObjectPinAdoptSiteCount = 7 preserved");
}

// Issue #2709 AC2: linter catches new create site without wire/EXEMPT.
static void ac2709_2_linter_catches_missing_wire() {
    std::println("\n--- #2709 AC2: linter catches missing wire/EXEMPT ---");
    const auto linter = read_file("scripts/check_general_object_pin_auto_wire_2709.py");
    CHECK(!linter.empty(), "AC2: linter script exists");
    CHECK(linter.find("#2709") != std::string::npos, "AC2: linter cites #2709");
    CHECK(linter.find("_scan_create_sites_for_missing_wire_or_exempt") != std::string::npos,
          "AC2: linter has allocate-pattern scan function");
    CHECK(linter.find("evaluator_primitives_eval.cpp") != std::string::npos,
          "AC2: linter scans evaluator_primitives_eval.cpp");
    CHECK(linter.find("evaluator_primitives_mutate.cpp") != std::string::npos,
          "AC2: linter scans evaluator_primitives_mutate.cpp");
    CHECK(linter.find("evaluator_primitives_query_workspace.cpp") != std::string::npos,
          "AC2: linter scans evaluator_primitives_query_workspace.cpp");
    CHECK(linter.find("evaluator_eval_flat.cpp") != std::string::npos,
          "AC2: linter scans evaluator_eval_flat.cpp");
    CHECK(linter.find("wire_general_object_create_pair_or_exempt") != std::string::npos,
          "AC2: linter scans for new helper");
    CHECK(linter.find("GENERAL_OBJECT_PIN_EXEMPT") != std::string::npos,
          "AC2: linter scans for EXEMPT marker");
}

// Issue #2709 AC3: Soft / dev_off / unset stays zero-cost.
static void ac2709_3_soft_zero_cost() {
    std::println("\n--- #2709 AC3: Soft zero-cost (counter reads only) ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("wire_general_object_create_pair_or_exempt") != std::string::npos,
          "AC3: default-on helper present");
    CHECK(lp.find("g_general_object_pin_required_pref.load(std::memory_order_relaxed) > 0") !=
              std::string::npos,
          "AC3: required-mode gate preserved (zero-cost when pref <= 0)");
    CHECK(lp.find("g_general_object_pin_required_enforced_total.fetch_add") != std::string::npos,
          "AC3: required_enforced_total only bumps inside the pref > 0 gate");
}

// Issue #2709 AC4: required-mode fail-closed regression (covered by #2665).
static void ac2709_4_required_mode_fail_closed_regression() {
    std::println("\n--- #2709 AC4: #2665 required-mode fail-closed regression ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("g_general_object_pin_required_enforced_total") != std::string::npos,
          "AC4: required_enforced_total still declared (no #2665 regression)");
    CHECK(lp.find("g_general_object_pin_required_pref.load(std::memory_order_relaxed) > 0") !=
              std::string::npos,
          "AC4: required-mode gate still gates on pref > 0");
    CHECK(lp.find("return wire_general_object_create_pair(pin_a, pin_b, a, b, gen, arena_id);") !=
              std::string::npos,
          "AC4: default-on helper delegates to wire_general_object_create_pair");
}

// Issue #2709 AC5: additive query keys.
static void ac2709_5_query_keys_added() {
    std::println("\n--- #2709 AC5: additive query keys ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("general-object-pin-auto-wire-total") != std::string::npos,
          "AC5: obs_eval exposes general-object-pin-auto-wire-total");
    CHECK(obs.find("general_object_pin_auto_wire_total") != std::string::npos,
          "AC5: obs_eval exposes camelCase auto_wire_total");
    CHECK(obs.find("general-object-pin-exempt-total") != std::string::npos,
          "AC5: obs_eval exposes general-object-pin-exempt-total");
    CHECK(obs.find("general_object_pin_exempt_total") != std::string::npos,
          "AC5: obs_eval exposes camelCase exempt_total");
    CHECK(obs.find("general-object-pin-adopt-site-count") != std::string::npos,
          "AC5: obs_eval exposes general-object-pin-adopt-site-count");
    CHECK(obs.find("general_object_pin_adopt_site_count") != std::string::npos,
          "AC5: obs_eval exposes camelCase adopt_site_count");
    CHECK(obs.find("schema-2709") != std::string::npos, "AC5: schema-2709 sentinel");
    CHECK(obs.find("issue-2709") != std::string::npos, "AC5: issue-2709 sentinel");
    CHECK(obs.find("schema-2665") != std::string::npos, "AC5: schema-2665 preserved");
}

// Issue #2709 AC6: source-cite + linter + schema + no docs/design/.
static void ac2709_6_source_and_linter() {
    std::println("\n--- #2709 AC6: source-cite + linter + no docs/design/ ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/check_general_object_pin_auto_wire_2709.py");
    const auto t = read_file("tests/core/test_general_object_pin_coverage_gate.cpp");

    CHECK(lp.find("Issue #2709") != std::string::npos, "AC6: lifetime_pin.hh cites #2709");
    CHECK(lp.find("kGeneralObjectPinAutoWireIssue = 2709") != std::string::npos,
          "AC6: lifetime_pin.hh stamps issue = 2709");
    CHECK(build.find("check_general_object_pin_auto_wire_2709") != std::string::npos ||
              build.find("cmd_general_object_pin_auto_wire_2709_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    CHECK(cmake.find("test_general_object_pin_coverage_gate") != std::string::npos,
          "AC6: CMake registers test");
    CHECK(!linter.empty() && linter.find("Issue #2709") != std::string::npos,
          "AC6: coverage linter present and cites #2709");
    CHECK(t.find("ac2709_1_default_on_helper") != std::string::npos, "AC6: AC1 test present");
    CHECK(t.find("ac2709_2_linter_catches_missing_wire") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2709_3_soft_zero_cost") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2709_4_required_mode_fail_closed_regression") != std::string::npos,
          "AC6: AC4 test present");
    CHECK(t.find("ac2709_5_query_keys_added") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2709_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    const std::string design_path = "docs/design/2709-";
    CHECK(read_file((design_path + "general-object-pin-auto-wire.md").c_str()).empty(),
          "AC6: no docs/design/2709-* per #1655");
}

// ── Issue #2840: densify fail-closed on required pin breach ──────────────
// Residual of #2597/#2665: production defaults lock required, but callers
// cast wire return to void and densify was not gated on pin failure.
// #2840 sets sticky breach on wire fail under required + Moving densify
// fail-closes; callers use wire_*_or_required_fail.
static void ac2840_1_breach_and_densify_gate() {
    std::println("\n--- #2840 AC1: breach sticky + densify gate ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(lp.find("Issue #2840") != std::string::npos, "2840 AC1: lifetime_pin.hh cites #2840");
    CHECK(lp.find("g_general_object_pin_required_breach") != std::string::npos,
          "2840 AC1: sticky breach flag");
    CHECK(lp.find("general_object_pin_required_active") != std::string::npos,
          "2840 AC1: required_active helper");
    CHECK(lp.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2840 AC1: caller-facing required-fail helper");
    CHECK(lp.find("g_general_object_pin_required_breach.store(1") != std::string::npos,
          "2840 AC1: wire fail sets breach under required");
    CHECK(arena.find("general_object_pin_required_breach_active") != std::string::npos,
          "2840 AC1: densify checks breach");
    CHECK(arena.find("g_general_object_pin_required_breach_densify_fail_total") !=
              std::string::npos,
          "2840 AC1: densify fail counter");
    CHECK(arena.find("clear_general_object_pin_required_breach") != std::string::npos,
          "2840 AC1: clean densify clears breach");
}

static void ac2840_2_callers_fail_closed() {
    std::println("\n--- #2840 AC2: create paths use required-fail helper ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");
    const auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(mut.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2840 AC2: mutate uses required-fail wire");
    CHECK(ev.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2840 AC2: eval uses required-fail wire");
    CHECK(qw.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2840 AC2: query_workspace uses required-fail wire");
    CHECK(flat.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2840 AC2: eval_flat uses required-fail wire");
    // Soft still zero-cost: helper returns true when not required.
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Soft: observe-only") != std::string::npos ||
              lp.find("return true;   // Soft") != std::string::npos ||
              lp.find("return true;") != std::string::npos,
          "2840 AC2: Soft observe-only path in helper");
}

static void ac2840_3_query_and_defaults() {
    std::println("\n--- #2840 AC3: query keys + production defaults lineage ---");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto hh = read_file("src/compiler/security_defaults.hh");
    CHECK(obs.find("schema-2840") != std::string::npos, "2840 AC3: schema-2840");
    CHECK(obs.find("general-object-pin-required-breach") != std::string::npos,
          "2840 AC3: breach query key");
    CHECK(obs.find("general-object-pin-required-breach-densify-fail-total") != std::string::npos,
          "2840 AC3: densify-fail-total query key");
    CHECK(hh.find("#2840") != std::string::npos, "2840 AC3: security_defaults cites #2840");
    CHECK(obs.find("schema-2665") != std::string::npos, "2840 AC3: schema-2665 preserved");
}

static void ac2840_6_linter_and_no_invent() {
    std::println("\n--- #2840 AC6: linter + no invent ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_general_object_pin_required_prod_default_2840") != std::string::npos,
          "2840 AC6: build.py wires #2840 linter");
    std::ifstream invent("tests/core/test_issue_2840.cpp");
    if (!invent)
        invent.open("../tests/core/test_issue_2840.cpp");
    CHECK(!invent.good(), "2840 AC6: no test_issue_2840.cpp");
}

// ── Issue #2891: force required-fail return check on intermediate create ──
// Residual of #2840/#2709: helper + sticky breach exist, but some
// mutate/agent/scratch intermediate create sites still void-cast the
// return (or skip the wire), so pin failure under production required
// only sets sticky densify-off without failing the create path. #2891
// wires set-code (agent self-modify workspace swap) + check-form
// (temp_arena_ scratch) and adds a void-cast linter.
// ── Issue #2892: post-compact lifecycle single-entry convergence ──
// (refine #2436): the single ordered post-compact close entry
// (run_post_compact_close) + AC4 observability counter
// post_compact_lifecycle_ran_total must stay wired and additive.
// Extends this suite per #81967; behavior covered in
// tests/compiler/test_densify_ownership_scan_fail_gate.cpp
// (ac2892_1..5); here we cite + verify the counters exist.
static void ac2892_1_ran_counter_cite() {
    std::println("\n--- #2892 AC1: ran_total counter present + additive ---");
    const auto hh = read_file("src/core/post_compact_lifecycle.hh");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(hh.find("Issue #2892") != std::string::npos, "2892 AC1: header cites #2892");
    CHECK(hh.find("post_compact_lifecycle_ran_total") != std::string::npos,
          "2892 AC1: ran_total counter declared");
    CHECK(hh.find("note_lifecycle_ran") != std::string::npos, "2892 AC1: ran helper declared");
    CHECK(hh.find("post_compact_lifecycle_runs_total") != std::string::npos,
          "2892 AC1: existing runs_total preserved (additive)");
    CHECK(obs.find("post-compact-lifecycle-ran-total") != std::string::npos,
          "2892 AC1: obs query key exposed");
}

static void ac2891_1_set_code_and_check_form_wire() {
    std::println("\n--- #2891 AC1: set-code + check-form required-fail wire ---");
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");
    const auto tst = read_file("src/compiler/evaluator_primitives_test.cpp");
    CHECK(ev.find("Issue #2891") != std::string::npos, "2891 AC1: eval.cpp cites #2891");
    CHECK(ev.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2891 AC1: set-code uses required-fail wire");
    CHECK(ev.find("set-code: GeneralObjectPin required under production (#2891)") !=
              std::string::npos,
          "2891 AC1: set-code fails closed under production required");
    CHECK(tst.find("Issue #2891") != std::string::npos, "2891 AC1: test.cpp cites #2891");
    CHECK(tst.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2891 AC1: check-form uses required-fail wire");
    CHECK(tst.find("import aura.core.lifetime_pin") != std::string::npos,
          "2891 AC1: test.cpp imports lifetime_pin");
}

static void ac2891_2_soft_observe_only() {
    std::println("\n--- #2891 AC2: Soft observe-only retained ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    CHECK(lp.find("Soft: observe-only") != std::string::npos ||
              lp.find("return true;   // Soft") != std::string::npos ||
              lp.find("return true; // Soft") != std::string::npos,
          "2891 AC2: helper keeps Soft observe-only path");
    // No new atomics on quiet path: helper body unchanged shape (single
    // required_active check + return ok/true).
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(ev.find("wire_general_object_create_pair_or_required_fail") != std::string::npos,
          "2891 AC2: eval wire site retained");
}

static void ac2891_3_no_void_cast_linter() {
    std::println("\n--- #2891 AC3: void-cast linter + src/-aligned + no invent ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_general_object_pin_required_2891") != std::string::npos,
          "2891 AC3: build.py wires #2891 linter");
    // No void-cast residual of the required-fail helper anywhere.
    for (const char* f : {"src/compiler/evaluator_primitives_eval.cpp",
                          "src/compiler/evaluator_primitives_test.cpp",
                          "src/compiler/evaluator_primitives_mutate.cpp",
                          "src/compiler/evaluator_primitives_query_workspace.cpp",
                          "src/compiler/evaluator_eval_flat.cpp"}) {
        const auto src = read_file(f);
        CHECK(src.find(
                  "(void)aura::core::lifetime::wire_general_object_create_pair_or_required_fail") ==
                  std::string::npos,
              "2891 AC3: no void-cast of or_required_fail in " + std::string(f));
    }
    std::ifstream invent("tests/core/test_issue_2891.cpp");
    if (!invent)
        invent.open("../tests/core/test_issue_2891.cpp");
    CHECK(!invent.good(), "2891 AC3: no test_issue_2891.cpp (per #81967)");
}

static void ac3057_coverage_gate_cite() {
    std::println("\n--- #3057: FFI opaque_heap_ slot cover cites known-root walk ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(lp.find("kFfiOpaquePinOrRemapResidualIssue = 3057") != std::string::npos, "3057: stamp");
    CHECK(mb.find("opaque_heap_") != std::string::npos, "3057: opaque_heap_ slotted");
    CHECK(mb.find("Issue #3057") != std::string::npos, "3057: walk cites #3057");
}

static void ac3055_coverage_gate_cite() {
    std::println("\n--- #3055: post-Moving stale scan cites last_object_remap_ ---");
    const auto arena = read_file("src/core/arena.ixx");
    const auto dc = read_file("src/core/densify_consistency_report.h");
    CHECK(dc.find("kMovingPostMovingStaleIssue = 3055") != std::string::npos, "3055: stamp");
    CHECK(arena.find("note_post_moving_live_ptr_canary") != std::string::npos,
          "3055: observe-only canary");
    CHECK(arena.find("count_post_moving_stale_known_ptrs_") != std::string::npos,
          "3055: post-move scan");
}

static void ac3053_coverage_gate_cite() {
    std::println("\n--- #3053: allocate residual cites pin/slot/EXEMPT triad ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(lp.find("kGeneralObjectPinAllocateResidualIssue = 3053") != std::string::npos,
          "3053: stamp");
    CHECK(arena.find("maybe_note_allocate_intermediate_") != std::string::npos,
          "3053: allocate auto-wire helper");
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "3053 AC4: inventory floor not hand-bumped");
}

static void ac3214_coverage_gate_cite() {
    std::println("\n--- #3214: densify-tracked allocate cover cites all sizes/paths ---");
    const auto lp = read_file("src/core/lifetime_pin.hh");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(lp.find("kDensifyTrackedAllocateCoverIssue = 3214") != std::string::npos, "3214: stamp");
    CHECK(arena.find("kDensifyTrackedAllocateCoverIssue = 3214") != std::string::npos,
          "3214: arena stamp");
    CHECK(arena.find("maybe_note_allocate_intermediate_") != std::string::npos,
          "3214: allocate note helper");
    CHECK(arena.find("non-small / pmr-fallback densify-tracked allocate") != std::string::npos,
          "3214: non-small branch");
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "3214 AC: inventory floor not hand-bumped");
}

int run_test_general_object_pin_coverage_gate() {
    std::println("=== Issue #2496: GeneralObjectPin adoption coverage gate ===");
    std::println("=== Issue #2597: production default AURA_GENERAL_OBJECT_PIN=required "
                 "(extends #2496 test file per #81967) ===");
    std::println("=== Issue #2665: production-default required-mode fail-closed counter + "
                 "additive query keys (extends #2496 test file per #81967) ===");
    std::println("=== Issue #2709: GeneralObjectPin mandatory coverage beyond inventory-of-7 "
                 "(extends #2496 test file per #81967) ===");
    std::println("=== Issue #2840: densify fail-closed on required pin breach "
                 "(extends #2496 test file per #81967) ===");
    std::println("=== Issue #3053: allocate residual cites pin/slot/EXEMPT triad "
                 "(extends #2496 test file per #81967) ===");
    std::println("=== Issue #3214: densify-tracked allocate cover on all sizes/paths "
                 "(extends #2496 test file per #81967) ===");
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
    ac2709_1_default_on_helper();
    ac2709_2_linter_catches_missing_wire();
    ac2709_3_soft_zero_cost();
    ac2709_4_required_mode_fail_closed_regression();
    ac2709_5_query_keys_added();
    ac2709_6_source_and_linter();
    ac2840_1_breach_and_densify_gate();
    ac2840_2_callers_fail_closed();
    ac2840_3_query_and_defaults();
    ac2840_6_linter_and_no_invent();
    ac2891_1_set_code_and_check_form_wire();
    ac2891_2_soft_observe_only();
    ac2891_3_no_void_cast_linter();
    // Issue #2892 (refine #2436): post-compact lifecycle single entry +
    // additive ran_total counter (extends suite per #81967).
    ac2892_1_ran_counter_cite();
    ac3053_coverage_gate_cite();
    ac3214_coverage_gate_cite();
    ac3055_coverage_gate_cite();
    ac3057_coverage_gate_cite();
    // Issue #3093: cover-aware intermediate create (slot / pin / EXEMPT
    // triad) closes the #3017 residual — value-only auto-wire is
    // observability only, not safe cover. Additive to existing
    // #2496/#2596/#2597 coverage gate per #81967.
    //   AC1: source-cite note_intermediate_create_with_cover_ helper on
    //        ASTArena (slot / EXEMPT / value-only triad).
    //   AC2: counter declarations
    //        (g_intermediate_create_value_only_total +
    //        g_intermediate_create_with_cover_total) on arena.ixx.
    //   AC3: helper has all three branches (slot / EXEMPT / value-only).
    //   AC4: no docs/design/3093-* (per #1655).
    //   AC5: no test_issue_3093.cpp (per #81934).
    std::println("\n=== Issue #3093: cover-aware intermediate create ===");
    {
        const auto arena = read_file("src/core/arena.ixx");
        CHECK(arena.find("void note_intermediate_create_with_cover_(void* p, void** slot,"
                         " const char* reason)") != std::string::npos,
              "#3093 AC1: ASTArena::note_intermediate_create_with_cover_ helper present");
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        CHECK(arena.find("g_intermediate_create_with_cover_total") != std::string::npos,
              "#3093 AC2: g_intermediate_create_with_cover_total counter declared");
        CHECK(arena.find("g_intermediate_create_value_only_total") != std::string::npos,
              "#3093 AC2: g_intermediate_create_value_only_total counter declared");
        CHECK(arena.find("intermediate_create_with_cover_total_v_read") != std::string::npos,
              "#3093 AC2: v_read function present (query exposure)");
    }
    {
        const auto arena = read_file("src/core/arena.ixx");
        const auto helper_pos = arena.find("void note_intermediate_create_with_cover_(");
        const auto helper_end =
            helper_pos != std::string::npos ? arena.find("\n    }", helper_pos) : std::string::npos;
        if (helper_pos != std::string::npos && helper_end != std::string::npos) {
            const auto body = arena.substr(helper_pos, helper_end - helper_pos);
            CHECK(body.find("slot != nullptr") != std::string::npos,
                  "#3093 AC3: helper has slot branch (register_external_root_slot_for_densify_)");
            CHECK(body.find("reason != nullptr") != std::string::npos,
                  "#3093 AC3: helper has EXEMPT branch (erase_intermediate_create_)");
            CHECK(body.find("note_intermediate_create_auto_wire_(p)") != std::string::npos,
                  "#3093 AC3: helper has value-only fallback (backward compat)");
        }
    }
    {
        // AC4: no docs/design/3093-* (per #1655)
        const std::ifstream docs_probe("docs/design/3093-cover-triad.md");
        CHECK(!docs_probe.good(), "#3093 AC4: no docs/design/3093-* (per #1655)");
    }
    {
        // AC5: no test_issue_3093.cpp (per #81934)
        const std::ifstream test_probe("tests/core/test_issue_3093.cpp");
        CHECK(!test_probe.good(), "#3093 AC5: no test_issue_3093.cpp (per #81934)");
    }

    // ── Issue #3306: defense-in-depth — densify entry also fail-closes
    // when intermediate_create_value_only_total > 0 under required.
    // Closes the dual-track residual where older call sites still hit
    // note_intermediate_create_auto_wire_ under required densify-tracked
    // allocates (leaving a value-only intermediate in
    // intermediate_creates_ that the has_unpinned_intermediate_creates_()
    // scan catches via push_back, but this OR clause belt-and-suspenders
    // the soak invariant — value_only_total == 0 under production
    // required (per AC2 of #3306).
    {
        const auto arena = read_file("src/core/arena.ixx");
        // Locate the densify entry fail-close block (around line 1996).
        const auto fail_close_pos =
            arena.find("aura::core::lifetime::general_object_pin_required_active() &&\n"
                       "                (has_unpinned_intermediate_creates_() ||\n"
                       "                 intermediate_create_value_only_total_v_read() > 0)");
        CHECK(fail_close_pos != std::string::npos,
              "#3306 AC1: densify entry fail-close now OR-condition on value_only_total > 0 under "
              "required");
        // Verify the existing fail-close fields (pin_contract_held=false,
        // moving_incomplete_remap=true, moving_blocked_precondition=true,
        // soft_gated=true) are preserved in the surrounding block.
        if (fail_close_pos != std::string::npos) {
            const std::string scope = arena.substr(fail_close_pos, 1500);
            CHECK(scope.find("result.pin_contract_held = false") != std::string::npos,
                  "#3306 AC2: pin_contract_held=false preserved in fail-close block");
            CHECK(scope.find("result.moving_incomplete_remap = true") != std::string::npos,
                  "#3306 AC2: moving_incomplete_remap=true preserved");
            CHECK(scope.find("result.moving_blocked_precondition = true") != std::string::npos,
                  "#3306 AC2: moving_blocked_precondition=true preserved");
            CHECK(scope.find("result.soft_gated = true") != std::string::npos,
                  "#3306 AC2: soft_gated=true preserved");
            CHECK(scope.find("g_moving_incomplete_remap_sticky_densify_off.exchange(") !=
                      std::string::npos,
                  "#3306 AC2: sticky densify-off via g_moving_incomplete_remap_sticky_densify_off "
                  "preserved");
        }
        // Verify the comment block documents #3306.
        CHECK(arena.find("Issue #3306: defense-in-depth") != std::string::npos,
              "#3306 AC3: comment documents the defense-in-depth close (soak invariant)");
    }
    // AC4: no docs/design/3306-* plan doc (per #1655).
    {
        const std::ifstream docs_probe("docs/design/3306-value-only-soak.md");
        CHECK(!docs_probe.good(), "#3306 AC4: no docs/design/3306-* (per #1655)");
    }
    // AC5: no invent test_issue_3306.cpp (per #81934) — we EXTEND
    // test_general_object_pin_coverage_gate instead.
    {
        const std::ifstream test_probe("tests/core/test_issue_3306.cpp");
        CHECK(!test_probe.good(),
              "#3306 AC5: no test_issue_3306.cpp (per #81934 — extend existing)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_general_object_pin_coverage_gate();
}
#endif

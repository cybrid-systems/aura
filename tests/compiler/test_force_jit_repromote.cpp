// @category: unit
// @reason: Issue #2502 — auto re-promote force-JIT regions after stable
//          recovery window (N clean reemits + StormLevel::None).
//          Issue #2895 — last success coverage + partial re-promote knobs
//          (refine #2502/#2601).
//
//   #2502 AC1: force-JIT Defuse → N successful reemits, no storm → bit cleared
//   #2502 AC2: storm active or new fail reason in window → no re-promote
//   #2502 AC3: on_reload_success still clears all (existing contract)
//   #2502 AC4: additive metrics + query keys (schema-2502)
//   #2502 AC5: source-cite + unit test isolation (reset helpers)
//   #2895 AC1: partial clear — covered bit N only; other force bits retained
//   #2895 AC2: default policy preserves #2502 require-pending-idle / wholesale
//   #2895 AC3: query last-reemit-success-region-mask + partial total
//   #2895 AC4: storm active → no repromote (existing guards)
//   #2895 AC5: source-cite coverage + knobs

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"

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

static std::int64_t href(CompilerService& cs, const char* query, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Light-link detection (#2687 AC5 pattern): under light link the
// stable-func-id map is a weak stub returning 0 (aura_jit_bridge_stub.cpp),
// so named closures get sid==0 and map-dependent behavioral assertions
// cannot hold. Probe with a throwaway name, then clear the map so the
// probe never leaks into full-JIT runs. AC4 additionally depends on
// CompilerService::eval engine:metrics queries that are not wired in
// light link (CompilerService symbols live in libaura_test_objects only)
// — under light link the query path crashes, so AC4's behavioral asserts
// become best-effort while AC5 source-cite always runs.
static bool light_link_env() {
    int preserved = -1;
    const auto sid = aura_get_or_preserve_stable_func_id("__light_probe_2502__", &preserved);
    aura_clear_stable_func_id_map();
    return sid == 0 && preserved == 0;
}

static void clear_idle(aura::compiler::HotUpdateRegistry& reg) {
    reg.on_reload_success();
    while (reg.reload_recovery_state().pending_dirty_count > 0)
        reg.on_recovery_pending_dirty_dec();
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
    reg.reset_reemit_boundary_handshake_for_test();
    reg.reset_force_jit_repromote_for_test();
    reg.on_reload_success();
}

// ── AC1: N clean reemits re-promote without full module reload ──
static void ac1_repromote_after_window() {
    std::println("\n--- #2502 AC1: force-JIT Defuse → N clean reemits → mask clear ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    reg.set_force_jit_repromote_window(3);

    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    const auto defuse_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse);
    CHECK((reg.reload_recovery_state().force_jit_regions_mask & defuse_bit) != 0,
          "AC1: Defuse bit set");
    CHECK(reg.force_jit_stable_successes() == 0, "AC1: streak 0 after demotion");
    const auto rep0 = reg.force_jit_repromote_total();

    // Soft zero-cost when already demoted but successes partial: 1,2 of 3.
    reg.on_reemit_pipeline_call(/*candidates=*/1, /*successes=*/1);
    CHECK(reg.force_jit_stable_successes() == 1, "AC1: streak 1");
    CHECK((reg.reload_recovery_state().force_jit_regions_mask & defuse_bit) != 0,
          "AC1: still demoted after 1");
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 2, "AC1: streak 2");
    CHECK(reg.reload_recovery_state().force_jit_regions_mask != 0, "AC1: still demoted after 2");

    // 3rd clean success → re-promote (no on_reload_success required).
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == 0, "AC1: mask cleared on window");
    CHECK(reg.force_jit_repromote_total() == rep0 + 1, "AC1: repromote_total +1");
    CHECK(reg.last_force_jit_repromote_reason() == static_cast<std::uint8_t>(AotReloadFail::Defuse),
          "AC1: last repromote reason Defuse");
    CHECK(reg.force_jit_stable_successes() == 0, "AC1: streak reset after repromote");

    // Soft zero-cost: further successes with mask 0 keep streak 0.
    reg.on_reemit_pipeline_call(2, 2);
    CHECK(reg.force_jit_stable_successes() == 0, "AC1: zero-cost idle streak");
    CHECK(reg.force_jit_repromote_total() == rep0 + 1, "AC1: no extra repromote when idle");
}

// ── AC2: storm or new fail blocks re-promote ──
static void ac2_storm_or_fail_blocks() {
    std::println("\n--- #2502 AC2: storm / new fail → no re-promote ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    reg.set_force_jit_repromote_window(3);

    // Storm active resets streak and never clears mask.
    reg.on_force_jit_for_reason(AotReloadFail::Version);
    const auto version_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Version);
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 2, "AC2: pre-storm streak 2");
    reg.set_shape_storm_active(true);
    const auto rep0 = reg.force_jit_repromote_total();
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: storm resets streak");
    CHECK((reg.reload_recovery_state().force_jit_regions_mask & version_bit) != 0,
          "AC2: mask stays under storm");
    CHECK(reg.force_jit_repromote_total() == rep0, "AC2: no repromote under storm");
    reg.set_shape_storm_active(false);

    // Rebuild streak then new force-JIT reason resets window.
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 2, "AC2: streak rebuild 2");
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: new force-JIT resets streak");
    CHECK(reg.reload_recovery_state().force_jit_regions_mask != 0, "AC2: still demoted");

    // Zero-success reemit resets streak; rollback resets streak.
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 1, "AC2: streak 1");
    reg.on_reemit_pipeline_call(1, /*successes=*/0);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: failed reemit resets");
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reload_rollback(AotReloadFail::Region);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: rollback resets streak");

    // attempts_left non-zero blocks.
    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    reg.on_recovery_set_attempts_left(2);
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: attempts_left blocks streak");
    reg.on_recovery_set_attempts_left(0);

    // pending_dirty blocks when require_pending_idle (default).
    reg.on_recovery_pending_dirty_inc();
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 0, "AC2: pending dirty blocks");
    reg.on_recovery_pending_dirty_dec();

    clear_idle(reg);
}

// ── AC3: on_reload_success still wholesale-clears ──
static void ac3_reload_success_clears() {
    std::println("\n--- #2502 AC3: on_reload_success wholesale clear ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    reg.set_force_jit_repromote_window(8); // high window so reemit won't clear
    reg.on_force_jit_for_reason(AotReloadFail::Region);
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    CHECK(reg.reload_recovery_state().force_jit_regions_mask != 0, "AC3: multi-bit mask set");
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 1, "AC3: streak advanced");
    reg.on_reload_success();
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == 0, "AC3: mask cleared");
    CHECK(reg.force_jit_stable_successes() == 0, "AC3: streak cleared with success");
}

// ── AC4: query keys additive ──
static void ac4_query_keys() {
    std::println("\n--- #2502 AC4: additive metrics + query keys ---");
    if (light_link_env()) {
        std::println("  (light link: CompilerService engine:metrics queries not wired → "
                     "AC4 behavioral asserts best-effort, source-cite kept)");
        return;
    }
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_idle(reg);
    reg.set_force_jit_repromote_window(2);
    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1); // window=2 → re-promote

    CHECK(href(cs, "query:reload-recovery-state", "schema-2502") == 2502, "AC4: schema-2502");
    CHECK(href(cs, "query:reload-recovery-state", "issue-2502") == 2502, "AC4: issue-2502");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-repromote-total") >= 1,
          "AC4: force-jit-repromote-total");
    CHECK(href(cs, "query:reload-recovery-state", "last-force-jit-repromote-reason") ==
              static_cast<std::int64_t>(AotReloadFail::Defuse),
          "AC4: last-force-jit-repromote-reason");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-repromote-window") == 2,
          "AC4: force-jit-repromote-window");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-stable-successes") >= 0,
          "AC4: force-jit-stable-successes present");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-repromote-require-pending-idle") == 1,
          "AC4: require-pending-idle default 1");
    CHECK(href(cs, "query:reload-recovery-state", "last-force-jit-repromote-at-epoch-notify") >= 0,
          "AC4: last-force-jit-repromote-at-epoch-notify");
    // Cross-link on hot-update surface.
    CHECK(href(cs, "query:hot-update-registry-stats", "schema-2502") == 2502,
          "AC4: schema-2502 on hot-update surface");
    CHECK(href(cs, "query:hot-update-registry-stats", "force-jit-repromote-total") >= 1,
          "AC4: repromote total on hot-update surface");

    // C snapshot agrees.
    aura_reload_recovery_snapshot snap{};
    aura_hot_update_reload_recovery_get_snapshot(&snap);
    CHECK(snap.schema_2502 == 2502, "AC4: C snap schema_2502");
    CHECK(snap.force_jit_repromote_total >= 1, "AC4: C snap repromote total");
    CHECK(snap.force_jit_regions_mask == 0, "AC4: C snap mask 0 after repromote");
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2502 AC5: source-cite + CMake gate ---");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(hh.find("maybe_force_jit_repromote_on_clean_success") != std::string::npos,
          "AC5: maybe_force_jit_repromote in hh");
    CHECK(hh.find("force_jit_repromote_total_") != std::string::npos,
          "AC5: force_jit_repromote_total_ field");
    CHECK(hh.find("2502") != std::string::npos, "AC5: #2502 cite in hh");
    CHECK(cpp.find("maybe_force_jit_repromote_on_clean_success") != std::string::npos,
          "AC5: re-promote impl in cpp");
    CHECK(cpp.find("on_reemit_pipeline_call") != std::string::npos &&
              cpp.find("maybe_force_jit_repromote_on_clean_success") != std::string::npos,
          "AC5: reemit path wires re-promote");
    CHECK(mut.find("force-jit-repromote-total") != std::string::npos,
          "AC5: query key force-jit-repromote-total");
    CHECK(mut.find("schema-2502") != std::string::npos, "AC5: schema-2502 in mutate");
    CHECK(cmake.find("test_force_jit_repromote") != std::string::npos, "AC5: cmake target");
}

// ── #2895 AC1: partial re-promote clears only covered bits ──
static void ac2895_partial_clear_covered_only() {
    std::println("\n--- #2895 AC1: partial clear covered bit; retain uncovered ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    reg.set_force_jit_repromote_window(2);
    reg.set_force_jit_repromote_only_covered_bits(true);

    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    const auto defuse_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse);
    const auto env_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Env);
    const auto both = defuse_bit | env_bit;
    CHECK((reg.reload_recovery_state().force_jit_regions_mask & both) == both,
          "2895 AC1: Defuse+Env demoted");

    // Agent/bridge: only Defuse recovered.
    reg.note_reemit_success_coverage(defuse_bit);
    CHECK(reg.last_reemit_success_region_mask() == defuse_bit,
          "2895 AC1: last success mask = Defuse");

    const auto part0 = reg.force_jit_repromote_partial_total();
    const auto full0 = reg.force_jit_repromote_total();
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1); // window=2 → partial clear

    const auto mask = reg.reload_recovery_state().force_jit_regions_mask;
    CHECK((mask & defuse_bit) == 0, "2895 AC1: Defuse bit cleared");
    CHECK((mask & env_bit) != 0, "2895 AC1: Env bit retained (not covered)");
    CHECK(reg.force_jit_repromote_partial_total() == part0 + 1, "2895 AC1: partial_total +1");
    CHECK(reg.force_jit_repromote_total() == full0, "2895 AC1: full repromote_total unchanged");
    CHECK(reg.force_jit_stable_successes() == 0, "2895 AC1: streak reset after partial");

    clear_idle(reg);
}

// ── #2895 AC2: default policy preserves #2502 wholesale + require-pending-idle ──
static void ac2895_default_preserves_2502() {
    std::println("\n--- #2895 AC2: default knobs preserve #2502 wholesale clear ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    // Defaults after reset: only_covered=0, require_pending_idle=1.
    CHECK(!reg.force_jit_repromote_only_covered_bits(), "2895 AC2: only_covered default off");
    CHECK(reg.force_jit_repromote_require_pending_idle(),
          "2895 AC2: require_pending_idle default on");

    reg.set_force_jit_repromote_window(2);
    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    // Even with narrower coverage note, default wholesale still clears all.
    const auto defuse_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse);
    reg.note_reemit_success_coverage(defuse_bit);
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == 0,
          "2895 AC2: default wholesale clears all bits");
    CHECK(reg.force_jit_repromote_partial_total() == 0,
          "2895 AC2: partial_total stays 0 under default");

    // require_pending_idle still blocks streak (no silent change).
    clear_idle(reg);
    reg.set_force_jit_repromote_window(3);
    reg.on_force_jit_for_reason(AotReloadFail::Version);
    reg.on_recovery_pending_dirty_inc();
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 0, "2895 AC2: pending dirty still blocks streak");
    reg.on_recovery_pending_dirty_dec();

    clear_idle(reg);
}

// ── #2895 AC3: query surface ──
static void ac2895_query_surface() {
    std::println("\n--- #2895 AC3: query last-success mask + partial counter ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_idle(reg);
    reg.set_force_jit_repromote_window(2);
    reg.set_force_jit_repromote_only_covered_bits(true);
    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    reg.on_force_jit_for_reason(AotReloadFail::Linear);
    const auto defuse_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse);
    reg.note_reemit_success_coverage(defuse_bit);
    reg.on_reemit_pipeline_call(1, 1);
    reg.on_reemit_pipeline_call(1, 1);

    CHECK(href(cs, "query:reload-recovery-state", "schema-2895") == 2895, "2895 AC3: schema-2895");
    CHECK(href(cs, "query:reload-recovery-state", "issue-2895") == 2895, "2895 AC3: issue-2895");
    CHECK(href(cs, "query:reload-recovery-state", "last-reemit-success-region-mask") ==
              static_cast<std::int64_t>(defuse_bit),
          "2895 AC3: last-reemit-success-region-mask");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-repromote-partial-total") >= 1,
          "2895 AC3: force-jit-repromote-partial-total");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-repromote-only-covered-bits") == 1,
          "2895 AC3: only-covered-bits knob");
    // Prior lineage preserved.
    CHECK(href(cs, "query:reload-recovery-state", "schema-2502") == 2502,
          "2895 AC3: schema-2502 preserved");
    CHECK(href(cs, "query:hot-update-registry-stats", "schema-2895") == 2895,
          "2895 AC3: schema-2895 on hot-update surface");

    aura_reload_recovery_snapshot snap{};
    aura_hot_update_reload_recovery_get_snapshot(&snap);
    CHECK(snap.schema_2895 == 2895, "2895 AC3: C snap schema_2895");
    CHECK(snap.force_jit_repromote_partial_total >= 1, "2895 AC3: C snap partial total");
    CHECK(static_cast<std::uint64_t>(snap.last_reemit_success_region_mask) == defuse_bit,
          "2895 AC3: C snap last success mask");

    clear_idle(reg);
}

// ── #2895 AC4: storm still blocks (existing guards) ──
static void ac2895_storm_blocks() {
    std::println("\n--- #2895 AC4: storm active → no partial/full repromote ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_idle(reg);
    reg.set_force_jit_repromote_window(2);
    reg.set_force_jit_repromote_only_covered_bits(true);
    reg.on_force_jit_for_reason(AotReloadFail::Defuse);
    const auto defuse_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse);
    reg.note_reemit_success_coverage(defuse_bit);
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 1, "2895 AC4: streak 1 pre-storm");
    reg.set_shape_storm_active(true);
    const auto part0 = reg.force_jit_repromote_partial_total();
    const auto full0 = reg.force_jit_repromote_total();
    reg.on_reemit_pipeline_call(1, 1);
    CHECK(reg.force_jit_stable_successes() == 0, "2895 AC4: storm resets streak");
    CHECK((reg.reload_recovery_state().force_jit_regions_mask & defuse_bit) != 0,
          "2895 AC4: mask stays under storm");
    CHECK(reg.force_jit_repromote_partial_total() == part0, "2895 AC4: no partial under storm");
    CHECK(reg.force_jit_repromote_total() == full0, "2895 AC4: no full under storm");
    reg.set_shape_storm_active(false);
    clear_idle(reg);
}

// ── #2895 AC5: source-cite ──
static void ac2895_source_cite() {
    std::println("\n--- #2895 AC5: source-cite coverage + knobs ---");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(hh.find("last_reemit_success_region_mask") != std::string::npos,
          "2895 AC5: last_reemit_success_region_mask in hh");
    CHECK(hh.find("force_jit_repromote_only_covered_bits") != std::string::npos,
          "2895 AC5: only_covered_bits in hh");
    CHECK(hh.find("force_jit_repromote_partial_total") != std::string::npos,
          "2895 AC5: partial_total in hh");
    CHECK(hh.find("note_reemit_success_coverage") != std::string::npos,
          "2895 AC5: note_reemit_success_coverage API");
    CHECK(hh.find("2895") != std::string::npos, "2895 AC5: #2895 cite in hh");
    CHECK(cpp.find("force_jit_repromote_partial_total_") != std::string::npos,
          "2895 AC5: partial counter in cpp");
    CHECK(cpp.find("repromote_only_covered_bits") != std::string::npos ||
              cpp.find("force_jit_repromote_only_covered_bits_") != std::string::npos,
          "2895 AC5: partial policy branch in cpp");
    CHECK(cpp.find("last_reemit_success_region_mask_") != std::string::npos,
          "2895 AC5: last success stamp in cpp");
    CHECK(mut.find("last-reemit-success-region-mask") != std::string::npos,
          "2895 AC5: query key last-reemit-success-region-mask");
    CHECK(mut.find("force-jit-repromote-partial-total") != std::string::npos,
          "2895 AC5: query key partial-total");
    CHECK(mut.find("schema-2895") != std::string::npos, "2895 AC5: schema-2895 in mutate");
    // Soft / no force-JIT: zero cost path still present.
    CHECK(cpp.find("force_jit_regions_mask_") != std::string::npos &&
              cpp.find("mask == 0") != std::string::npos,
          "2895 AC5: zero-cost idle short-circuit retained");
}

} // namespace

int run_test_force_jit_repromote() {
    std::println("test_force_jit_repromote");
    ac1_repromote_after_window();
    ac2_storm_or_fail_blocks();
    ac3_reload_success_clears();
    ac4_query_keys();
    ac5_source_and_gate();
    ac2895_partial_clear_covered_only();
    ac2895_default_preserves_2502();
    ac2895_query_surface();
    ac2895_storm_blocks();
    ac2895_source_cite();
    if (g_failed)
        return 1;
    std::println("force-jit re-promote #2502/#2895: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_force_jit_repromote();
}
#endif

// @category: unit
// @reason: Issue #2502 — auto re-promote force-JIT regions after stable
//          recovery window (N clean reemits + StormLevel::None).
//
//   AC1: force-JIT Defuse → N successful reemits, no storm → bit cleared
//   AC2: storm active or new fail reason in window → no re-promote
//   AC3: on_reload_success still clears all (existing contract)
//   AC4: additive metrics + query keys (schema-2502)
//   AC5: source-cite + unit test isolation (reset helpers)

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
    const auto defuse_bit = static_cast<std::uint64_t>(1)
                            << static_cast<unsigned>(AotReloadFail::Defuse);
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
    const auto version_bit = static_cast<std::uint64_t>(1)
                             << static_cast<unsigned>(AotReloadFail::Version);
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

} // namespace

int run_test_force_jit_repromote() {
    std::println("test_force_jit_repromote");
    ac1_repromote_after_window();
    ac2_storm_or_fail_blocks();
    ac3_reload_success_clears();
    ac4_query_keys();
    ac5_source_and_gate();
    if (g_failed)
        return 1;
    std::println("force-jit re-promote #2502: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_force_jit_repromote();
}
#endif

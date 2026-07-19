// @category: unit
// @reason: Issue #1478 MVP — linear_post_mutate_enforce + counters
// Restored/verified by Issue #1541 (scope-limited close deferred build).
//
//   AC1: linear_post_mutate_enforcements accessor returns 0 initially
//   AC2: linear_ownership_violation_prevented accessor returns 0 initially
//   AC3: helper returns true for NULL_ENV_ID (safety net)
//   AC4: helper returns true for invalid env_id (safety net)
//   AC5: NULL_ENV_ID doesn't bump counter
//   AC6: invalid env_id doesn't bump counter
//   AC7: linear_post_mutate_enforce on valid frame bumps counter
//   AC8: 1000-iter stress (monotonic)
//   AC9: violation_prevented stays 0 without Moved bindings (passive path)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1478_detail {

using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_enforce_count_initial() {
    std::println("\n--- AC1: linear_post_mutate_enforcements accessor 0 initially ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.test_linear_post_mutate_enforce_count() == 0,
          "linear_post_mutate_enforcements starts at 0");
}

static void ac2_violation_count_initial() {
    std::println("\n--- AC2: linear_ownership_violation_prevented accessor 0 initially ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.test_linear_ownership_violation_prevented_count() == 0,
          "linear_ownership_violation_prevented starts at 0");
}

static void ac3_null_env_safe() {
    std::println("\n--- AC3: helper returns true for NULL_ENV_ID ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.linear_post_mutate_enforce(NULL_ENV_ID) == true, "NULL_ENV_ID → true (safety net)");
}

static void ac4_invalid_env_safe() {
    std::println("\n--- AC4: helper returns true for invalid env_id ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Far beyond any allocated frame.
    constexpr auto kInvalid = static_cast<aura::compiler::EnvId>(1u << 30);
    CHECK(ev.linear_post_mutate_enforce(kInvalid) == true, "invalid env_id → true (safety net)");
}

static void ac5_null_no_bump() {
    std::println("\n--- AC5: NULL_ENV_ID doesn't bump counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
    (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
    CHECK(ev.test_linear_post_mutate_enforce_count() == c0,
          "NULL_ENV_ID does not bump enforcements counter");
}

static void ac6_invalid_no_bump() {
    std::println("\n--- AC6: invalid env_id doesn't bump counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    constexpr auto kInvalid = static_cast<aura::compiler::EnvId>(1u << 30);
    (void)ev.linear_post_mutate_enforce(kInvalid);
    CHECK(ev.test_linear_post_mutate_enforce_count() == c0,
          "invalid env_id does not bump enforcements counter");
}

static void ac7_valid_frame_bumps() {
    std::println("\n--- AC7: valid frame bumps enforcements (closure-path equivalent) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Allocate a live EnvFrame (Owned binding, not Moved).
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(1, make_int(1), /*Owned*/ 1);
    auto eid = ev.alloc_env_frame_from_env(src);
    CHECK(eid != NULL_ENV_ID, "allocated frame");
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    // Mirrors what closure_needs_safe_fallback does on apply with captures.
    CHECK(ev.linear_post_mutate_enforce(eid) == true, "Owned frame → true");
    CHECK(ev.test_linear_post_mutate_enforce_count() == c0 + 1,
          "valid frame bumps enforcements by 1");
    // Also exercise via eval path that applies closures (secondary coverage).
    CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    const auto c1 = ev.test_linear_post_mutate_enforce_count();
    (void)cs.eval("(g)");
    // apply may or may not bump depending on env_id capture path; count is monotonic.
    CHECK(ev.test_linear_post_mutate_enforce_count() >= c1, "after (g) enforcements monotonic");
}

static void ac8_stress_1000() {
    std::println("\n--- AC8: 1000-iter stress (monotonic) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(7, make_int(0), /*Owned*/ 1);
    auto eid = ev.alloc_env_frame_from_env(src);
    const auto c0 = ev.test_linear_post_mutate_enforce_count();
    bool all_ok = true;
    for (int i = 0; i < 1000; ++i) {
        if (!ev.linear_post_mutate_enforce(eid))
            all_ok = false;
    }
    CHECK(all_ok, "1000-iter all safe");
    const auto c1 = ev.test_linear_post_mutate_enforce_count();
    CHECK(c1 == c0 + 1000, "1000 enforces → +1000 counter");
    std::println("  counter {} → {}", c0, c1);
}

static void ac9_violation_passive_without_moved() {
    std::println("\n--- AC9: violation_prevented stays 0 without Moved bindings ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(3, make_int(3), /*Owned*/ 1);
    auto eid = ev.alloc_env_frame_from_env(src);
    const auto v0 = ev.test_linear_ownership_violation_prevented_count();
    for (int i = 0; i < 50; ++i)
        (void)ev.linear_post_mutate_enforce(eid);
    (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
    CHECK(ev.test_linear_ownership_violation_prevented_count() == v0,
          "no Moved bindings → violation_prevented unchanged");
}

// Issue #1638 extension: 3 new ACs (AC10/11/12) for the SoA EnvFrame
// dual-path consistency + mutation_log compact hooks. Source-driven
// smoke tests (the actual compact fires on heavy-mutation scenarios
// impractical to drive in a unit test; these verify the metrics
// surface + bump helpers are wired correctly + the public methods
// are callable from outside the Evaluator).

static void ac10_dual_path_metrics_reachable(CompilerService& cs) {
    auto& ev = cs.evaluator();
    // Newly added metrics start at 0 on a fresh service.
    const auto dp0 = ev.get_dual_path_stale_fallback_total();
    const auto ml0 = ev.get_mutation_log_compact_bytes_saved();
    const auto vd0 = ev.get_env_frame_version_drift_prevented();
    CHECK(dp0 == 0 && ml0 == 0 && vd0 == 0,
          "AC10: dual_path / compact-bytes / drift-prevented all start at 0");
}

static void ac11_ensure_dual_path_consistent_callable(CompilerService& cs) {
    auto& ev = cs.evaluator();
    // NULL_ENV_ID should be the safe net — returns true without bumping.
    const auto before = ev.get_env_frame_version_drift_prevented();
    (void)ev.ensure_env_frame_dual_path_consistent(NULL_ENV_ID, "ac11_null_env_id");
    const auto after = ev.get_env_frame_version_drift_prevented();
    CHECK(after == before, "AC11: NULL_ENV_ID does not bump env_frame_version_drift_prevented");
}

static void ac12_compact_mutation_log_callable(CompilerService& cs) {
    auto& ev = cs.evaluator();
    // No-op when mutation_log is empty (returns 0 bytes saved, no bump).
    const auto before = ev.get_mutation_log_compact_bytes_saved();
    ev.compact_mutation_log();
    const auto after = ev.get_mutation_log_compact_bytes_saved();
    CHECK(after == before, "AC12: compact_mutation_log on empty log is a no-op (monotonic metric)");
}

} // namespace aura_issue_1478_detail

int main() {
    using namespace aura_issue_1478_detail;
    std::println("=== Issue #1478: linear_post_mutate_enforce MVP (verified by #1541) ===");
    std::println(
        "=== + Issue #1638: SoA EnvFrame dual-path consistency extension (AC10/11/12) ===");
    ac1_enforce_count_initial();
    ac2_violation_count_initial();
    ac3_null_env_safe();
    ac4_invalid_env_safe();
    ac5_null_no_bump();
    ac6_invalid_no_bump();
    ac7_valid_frame_bumps();
    ac8_stress_1000();
    ac9_violation_passive_without_moved();
    {
        CompilerService cs;
        ac10_dual_path_metrics_reachable(cs);
        ac11_ensure_dual_path_consistent_callable(cs);
        ac12_compact_mutation_log_callable(cs);
    }
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

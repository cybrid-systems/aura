// test_mutation_hold_boundary_batch.cpp — thematic multi-TU batch
// Mutation hold / boundary / guard / steal×mutation ACs
// Stream A1 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_effect_epoch_mutation_unify_2149();
extern int run_test_guard_exit_occurrence_refresh_2144();
extern int run_test_mutation_contention_2040();
extern int run_test_mutation_guard_try_acquire_2124();
extern int run_test_mutation_hold_estimate_2405();
extern int run_test_mutation_hold_hard_timeout_2199();
extern int run_test_mutation_hold_live_2517();
extern int run_test_mutation_hold_slo_2349();
extern int run_test_mutation_log_pressure_2201();
extern int run_test_mutation_memory_blame_2196();
extern int run_test_outermost_exit_order_2120();
extern int run_test_post_steal_linear_revalidate_2197();
extern int run_test_predicate_memo_boundary_selective_2104();
extern int run_test_typed_mutation_audit_decision_2281();
extern int run_test_gc_defer_mutation_hold_2204();
extern int run_test_boundary_yield_steal_metrics_2119();
extern int run_test_depth_safe_mutation_boundary_steal_2115();
extern int run_test_mutation_safety_snapshot_steal_2184();
extern int run_test_orch_agent_mutation_boundary_2118();
extern int run_test_orch_soft_boundary_unified_2515();
extern int run_test_yield_while_mutation_held_2200();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_mutation_hold_boundary_batch (21 members) ===");

    std::println("\n──── test_effect_epoch_mutation_unify_2149 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_effect_epoch_mutation_unify_2149() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_effect_epoch_mutation_unify_2149 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_effect_epoch_mutation_unify_2149 ({} checks)", g_passed);
    }

    std::println("\n──── test_guard_exit_occurrence_refresh_2144 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_guard_exit_occurrence_refresh_2144() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_guard_exit_occurrence_refresh_2144 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_guard_exit_occurrence_refresh_2144 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_contention_2040 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_contention_2040() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_contention_2040 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_contention_2040 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_guard_try_acquire_2124 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_guard_try_acquire_2124() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mutation_guard_try_acquire_2124 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_guard_try_acquire_2124 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_hold_estimate_2405 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_estimate_2405() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_hold_estimate_2405 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_hold_estimate_2405 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_hold_hard_timeout_2199 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_hard_timeout_2199() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mutation_hold_hard_timeout_2199 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_hold_hard_timeout_2199 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_hold_live_2517 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_live_2517() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_hold_live_2517 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_hold_live_2517 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_hold_slo_2349 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_slo_2349() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_hold_slo_2349 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_hold_slo_2349 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_log_pressure_2201 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_log_pressure_2201() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_log_pressure_2201 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_log_pressure_2201 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_memory_blame_2196 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_memory_blame_2196() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_memory_blame_2196 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_memory_blame_2196 ({} checks)", g_passed);
    }

    std::println("\n──── test_outermost_exit_order_2120 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_outermost_exit_order_2120() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_outermost_exit_order_2120 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_outermost_exit_order_2120 ({} checks)", g_passed);
    }

    std::println("\n──── test_post_steal_linear_revalidate_2197 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_steal_linear_revalidate_2197() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_post_steal_linear_revalidate_2197 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_post_steal_linear_revalidate_2197 ({} checks)", g_passed);
    }

    std::println("\n──── test_predicate_memo_boundary_selective_2104 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_predicate_memo_boundary_selective_2104() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_predicate_memo_boundary_selective_2104 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_predicate_memo_boundary_selective_2104 ({} checks)", g_passed);
    }

    std::println("\n──── test_typed_mutation_audit_decision_2281 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_typed_mutation_audit_decision_2281() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_typed_mutation_audit_decision_2281 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_typed_mutation_audit_decision_2281 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_mutation_hold_2204 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_mutation_hold_2204() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_defer_mutation_hold_2204 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_mutation_hold_2204 ({} checks)", g_passed);
    }

    std::println("\n──── test_boundary_yield_steal_metrics_2119 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_boundary_yield_steal_metrics_2119() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_boundary_yield_steal_metrics_2119 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_boundary_yield_steal_metrics_2119 ({} checks)", g_passed);
    }

    std::println("\n──── test_depth_safe_mutation_boundary_steal_2115 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_depth_safe_mutation_boundary_steal_2115() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_depth_safe_mutation_boundary_steal_2115 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_depth_safe_mutation_boundary_steal_2115 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_mutation_safety_snapshot_steal_2184 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_safety_snapshot_steal_2184() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mutation_safety_snapshot_steal_2184 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_safety_snapshot_steal_2184 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_agent_mutation_boundary_2118 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_agent_mutation_boundary_2118() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_orch_agent_mutation_boundary_2118 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_agent_mutation_boundary_2118 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_soft_boundary_unified_2515 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_soft_boundary_unified_2515() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_orch_soft_boundary_unified_2515 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_soft_boundary_unified_2515 ({} checks)", g_passed);
    }

    std::println("\n──── test_yield_while_mutation_held_2200 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_yield_while_mutation_held_2200() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_yield_while_mutation_held_2200 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_yield_while_mutation_held_2200 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

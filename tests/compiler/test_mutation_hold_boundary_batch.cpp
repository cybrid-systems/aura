// test_mutation_hold_boundary_batch.cpp — thematic multi-TU batch
// Stream S4 disambiguated names.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_effect_epoch_mutation_unify();
extern int run_test_guard_exit_occurrence_refresh();
extern int run_test_mutation_contention();
extern int run_test_mutation_guard_try_acquire_unit();
extern int run_test_mutation_hold_estimate();
extern int run_test_mutation_hold_hard_timeout();
extern int run_test_mutation_hold_live();
extern int run_test_mutation_hold_slo();
extern int run_test_mutation_log_pressure();
extern int run_test_mutation_memory_blame();
extern int run_test_outermost_exit_order();
extern int run_test_post_steal_linear_revalidate();
extern int run_test_predicate_memo_boundary_selective();
extern int run_test_typed_mutation_audit_decision();
extern int run_test_gc_defer_mutation_hold();
extern int run_test_boundary_yield_steal_metrics();
extern int run_test_depth_safe_mutation_boundary_steal();
extern int run_test_mutation_safety_snapshot_steal();
extern int run_test_orch_agent_mutation_boundary();
extern int run_test_orch_soft_boundary_unified();
extern int run_test_yield_while_mutation_held();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_mutation_hold_boundary_batch (21 members) ===");

    std::println("\n──── test_effect_epoch_mutation_unify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_effect_epoch_mutation_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_effect_epoch_mutation_unify");
    } else {
        ++members_passed;
        std::println("OK test_effect_epoch_mutation_unify");
    }

    std::println("\n──── test_guard_exit_occurrence_refresh ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_guard_exit_occurrence_refresh() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_guard_exit_occurrence_refresh");
    } else {
        ++members_passed;
        std::println("OK test_guard_exit_occurrence_refresh");
    }

    std::println("\n──── test_mutation_contention ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_contention() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_contention");
    } else {
        ++members_passed;
        std::println("OK test_mutation_contention");
    }

    std::println("\n──── test_mutation_guard_try_acquire_unit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_guard_try_acquire_unit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_guard_try_acquire_unit");
    } else {
        ++members_passed;
        std::println("OK test_mutation_guard_try_acquire_unit");
    }

    std::println("\n──── test_mutation_hold_estimate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_estimate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_hold_estimate");
    } else {
        ++members_passed;
        std::println("OK test_mutation_hold_estimate");
    }

    std::println("\n──── test_mutation_hold_hard_timeout ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_hard_timeout() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_hold_hard_timeout");
    } else {
        ++members_passed;
        std::println("OK test_mutation_hold_hard_timeout");
    }

    std::println("\n──── test_mutation_hold_live ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_live() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_hold_live");
    } else {
        ++members_passed;
        std::println("OK test_mutation_hold_live");
    }

    std::println("\n──── test_mutation_hold_slo ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_hold_slo() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_hold_slo");
    } else {
        ++members_passed;
        std::println("OK test_mutation_hold_slo");
    }

    std::println("\n──── test_mutation_log_pressure ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_log_pressure() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_log_pressure");
    } else {
        ++members_passed;
        std::println("OK test_mutation_log_pressure");
    }

    std::println("\n──── test_mutation_memory_blame ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_memory_blame() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_memory_blame");
    } else {
        ++members_passed;
        std::println("OK test_mutation_memory_blame");
    }

    std::println("\n──── test_outermost_exit_order ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_outermost_exit_order() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_outermost_exit_order");
    } else {
        ++members_passed;
        std::println("OK test_outermost_exit_order");
    }

    std::println("\n──── test_post_steal_linear_revalidate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_steal_linear_revalidate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_post_steal_linear_revalidate");
    } else {
        ++members_passed;
        std::println("OK test_post_steal_linear_revalidate");
    }

    std::println("\n──── test_predicate_memo_boundary_selective ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_predicate_memo_boundary_selective() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_predicate_memo_boundary_selective");
    } else {
        ++members_passed;
        std::println("OK test_predicate_memo_boundary_selective");
    }

    std::println("\n──── test_typed_mutation_audit_decision ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_typed_mutation_audit_decision() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_typed_mutation_audit_decision");
    } else {
        ++members_passed;
        std::println("OK test_typed_mutation_audit_decision");
    }

    std::println("\n──── test_gc_defer_mutation_hold ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_mutation_hold() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_gc_defer_mutation_hold");
    } else {
        ++members_passed;
        std::println("OK test_gc_defer_mutation_hold");
    }

    std::println("\n──── test_boundary_yield_steal_metrics ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_boundary_yield_steal_metrics() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_boundary_yield_steal_metrics");
    } else {
        ++members_passed;
        std::println("OK test_boundary_yield_steal_metrics");
    }

    std::println("\n──── test_depth_safe_mutation_boundary_steal ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_depth_safe_mutation_boundary_steal() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_depth_safe_mutation_boundary_steal");
    } else {
        ++members_passed;
        std::println("OK test_depth_safe_mutation_boundary_steal");
    }

    std::println("\n──── test_mutation_safety_snapshot_steal ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_safety_snapshot_steal() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_mutation_safety_snapshot_steal");
    } else {
        ++members_passed;
        std::println("OK test_mutation_safety_snapshot_steal");
    }

    std::println("\n──── test_orch_agent_mutation_boundary ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_agent_mutation_boundary() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_orch_agent_mutation_boundary");
    } else {
        ++members_passed;
        std::println("OK test_orch_agent_mutation_boundary");
    }

    std::println("\n──── test_orch_soft_boundary_unified ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_soft_boundary_unified() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_orch_soft_boundary_unified");
    } else {
        ++members_passed;
        std::println("OK test_orch_soft_boundary_unified");
    }

    std::println("\n──── test_yield_while_mutation_held ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_yield_while_mutation_held() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_yield_while_mutation_held");
    } else {
        ++members_passed;
        std::println("OK test_yield_while_mutation_held");
    }

    std::println("\n=== {} ok, {} failed ===", members_passed, members_failed);
    return members_failed ? 1 : 0;
}

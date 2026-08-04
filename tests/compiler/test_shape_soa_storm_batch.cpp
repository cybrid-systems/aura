// test_shape_soa_storm_batch.cpp — thematic multi-TU batch
// Shape / SoA / storm isolation ACs
// Stream A2 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_dirty_aware_shape_linear_passes_2130();
extern int run_test_hot_pass_dirty_soa_2060();
extern int run_test_shape_compact_storm_isolation_2617();
extern int run_test_shape_high_mutation_storm_2433();
extern int run_test_shape_storm_adaptive_2526();
extern int run_test_soa_ban_residual_aos_bridge_2520();
extern int run_test_soa_dirty_aware_pipeline_2143();
extern int run_test_soa_generation_fence_2111();
extern int run_test_soa_partial_desync_gate_2181();
extern int run_test_soa_residual_production_smoke_2618();
extern int run_test_soa_single_entry_dirty_sync_2139();
extern int run_test_storm_isolation_2236();
extern int run_test_hot_children_columnar_2614();
extern int run_test_validate_post_restore_soa_2391();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_shape_soa_storm_batch (14 members) ===");

    std::println("\n──── test_dirty_aware_shape_linear_passes_2130 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dirty_aware_shape_linear_passes_2130() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dirty_aware_shape_linear_passes_2130 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dirty_aware_shape_linear_passes_2130 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_dirty_soa_2060 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_dirty_soa_2060() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_dirty_soa_2060 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_dirty_soa_2060 ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_compact_storm_isolation_2617 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_compact_storm_isolation_2617() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_shape_compact_storm_isolation_2617 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_compact_storm_isolation_2617 ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_high_mutation_storm_2433 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_high_mutation_storm_2433() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_shape_high_mutation_storm_2433 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_high_mutation_storm_2433 ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_storm_adaptive_2526 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_storm_adaptive_2526() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_storm_adaptive_2526 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_storm_adaptive_2526 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_ban_residual_aos_bridge_2520 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_ban_residual_aos_bridge_2520() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_soa_ban_residual_aos_bridge_2520 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_ban_residual_aos_bridge_2520 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_dirty_aware_pipeline_2143 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_dirty_aware_pipeline_2143() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_soa_dirty_aware_pipeline_2143 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_dirty_aware_pipeline_2143 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_generation_fence_2111 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_generation_fence_2111() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_generation_fence_2111 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_generation_fence_2111 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_partial_desync_gate_2181 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_partial_desync_gate_2181() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_partial_desync_gate_2181 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_partial_desync_gate_2181 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_residual_production_smoke_2618 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_residual_production_smoke_2618() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_soa_residual_production_smoke_2618 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_residual_production_smoke_2618 ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_single_entry_dirty_sync_2139 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_single_entry_dirty_sync_2139() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_soa_single_entry_dirty_sync_2139 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_single_entry_dirty_sync_2139 ({} checks)", g_passed);
    }

    std::println("\n──── test_storm_isolation_2236 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_storm_isolation_2236() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_storm_isolation_2236 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_storm_isolation_2236 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_children_columnar_2614 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_children_columnar_2614() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_children_columnar_2614 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_children_columnar_2614 ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_post_restore_soa_2391 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_post_restore_soa_2391() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_validate_post_restore_soa_2391 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_post_restore_soa_2391 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

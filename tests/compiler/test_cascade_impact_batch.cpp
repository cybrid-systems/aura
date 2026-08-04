// test_cascade_impact_batch.cpp — thematic multi-TU batch
// Cascade / adaptive thr / instr impact ACs (Stream A10e)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adaptive_cascade_depth_partial_thr_2209();
extern int run_test_adaptive_reverify_limit_2146();
extern int run_test_cascade_incremental_pass_suite_2044();
extern int run_test_cascade_skip_metrics_2106();
extern int run_test_dep_graph_hybrid_cascade_2110();
extern int run_test_frame_budget_cascade_isolation_2137();
extern int run_test_instr_impact_minimal_dirty_2126();
extern int run_test_instruction_level_impact_partial_2109();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_cascade_impact_batch (8 members) ===");

    std::println("\n──── test_adaptive_cascade_depth_partial_thr_2209 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_cascade_depth_partial_thr_2209() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_cascade_depth_partial_thr_2209 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_cascade_depth_partial_thr_2209 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_adaptive_reverify_limit_2146 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_reverify_limit_2146() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_reverify_limit_2146 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_reverify_limit_2146 ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_incremental_pass_suite_2044 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_incremental_pass_suite_2044() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_cascade_incremental_pass_suite_2044 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_incremental_pass_suite_2044 ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_skip_metrics_2106 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_skip_metrics_2106() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cascade_skip_metrics_2106 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_skip_metrics_2106 ({} checks)", g_passed);
    }

    std::println("\n──── test_dep_graph_hybrid_cascade_2110 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dep_graph_hybrid_cascade_2110() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dep_graph_hybrid_cascade_2110 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dep_graph_hybrid_cascade_2110 ({} checks)", g_passed);
    }

    std::println("\n──── test_frame_budget_cascade_isolation_2137 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_frame_budget_cascade_isolation_2137() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_frame_budget_cascade_isolation_2137 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_frame_budget_cascade_isolation_2137 ({} checks)", g_passed);
    }

    std::println("\n──── test_instr_impact_minimal_dirty_2126 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instr_impact_minimal_dirty_2126() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instr_impact_minimal_dirty_2126 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instr_impact_minimal_dirty_2126 ({} checks)", g_passed);
    }

    std::println("\n──── test_instruction_level_impact_partial_2109 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instruction_level_impact_partial_2109() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instruction_level_impact_partial_2109 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instruction_level_impact_partial_2109 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

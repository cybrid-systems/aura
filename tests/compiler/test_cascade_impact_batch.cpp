// test_cascade_impact_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adaptive_cascade_depth_partial_thr();
extern int run_test_adaptive_reverify_limit();
extern int run_test_cascade_incremental_pass_suite();
extern int run_test_cascade_skip_metrics();
extern int run_test_dep_graph_hybrid_cascade();
extern int run_test_frame_budget_cascade_isolation();
extern int run_test_instr_impact_minimal_dirty();
extern int run_test_instruction_level_impact_partial();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_cascade_impact_batch (8 members) ===");

    std::println("\n──── test_adaptive_cascade_depth_partial_thr ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_cascade_depth_partial_thr() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_cascade_depth_partial_thr ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_cascade_depth_partial_thr ({} checks)", g_passed);
    }

    std::println("\n──── test_adaptive_reverify_limit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_reverify_limit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_reverify_limit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_reverify_limit ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_incremental_pass_suite ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_incremental_pass_suite() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cascade_incremental_pass_suite ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_incremental_pass_suite ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_skip_metrics ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_skip_metrics() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cascade_skip_metrics ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_skip_metrics ({} checks)", g_passed);
    }

    std::println("\n──── test_dep_graph_hybrid_cascade ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dep_graph_hybrid_cascade() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dep_graph_hybrid_cascade ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dep_graph_hybrid_cascade ({} checks)", g_passed);
    }

    std::println("\n──── test_frame_budget_cascade_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_frame_budget_cascade_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_frame_budget_cascade_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_frame_budget_cascade_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_instr_impact_minimal_dirty ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instr_impact_minimal_dirty() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_instr_impact_minimal_dirty ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instr_impact_minimal_dirty ({} checks)", g_passed);
    }

    std::println("\n──── test_instruction_level_impact_partial ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instruction_level_impact_partial() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_instruction_level_impact_partial ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instruction_level_impact_partial ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// test_module_query_batch.cpp — thematic multi-TU batch
// Module load/rebind/export + query:* hygiene/index ACs
// Stream A4 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_module_export_display_2572();
extern int run_test_module_load_tail_export_2570();
extern int run_test_module_partition_map_2524();
extern int run_test_module_rebind_residual_2579();
extern int run_test_module_require_freevar_2566();
extern int run_test_query_and_replace_batch_2527();
extern int run_test_query_by_marker_provenance_2242();
extern int run_test_query_epoch_contract_2192();
extern int run_test_query_hygiene_default_2525();
extern int run_test_query_index_composite_2403();
extern int run_test_query_pattern_default_hygiene_2123();
extern int run_test_setcode_rebind_survive_2569();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_module_query_batch (12 members) ===");

    std::println("\n──── test_module_export_display_2572 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_export_display_2572() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_export_display_2572 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_export_display_2572 ({} checks)", g_passed);
    }

    std::println("\n──── test_module_load_tail_export_2570 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_load_tail_export_2570() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_load_tail_export_2570 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_load_tail_export_2570 ({} checks)", g_passed);
    }

    std::println("\n──── test_module_partition_map_2524 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_partition_map_2524() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_partition_map_2524 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_partition_map_2524 ({} checks)", g_passed);
    }

    std::println("\n──── test_module_rebind_residual_2579 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_rebind_residual_2579() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_rebind_residual_2579 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_rebind_residual_2579 ({} checks)", g_passed);
    }

    std::println("\n──── test_module_require_freevar_2566 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_require_freevar_2566() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_require_freevar_2566 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_require_freevar_2566 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_and_replace_batch_2527 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_and_replace_batch_2527() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_and_replace_batch_2527 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_and_replace_batch_2527 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_by_marker_provenance_2242 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_by_marker_provenance_2242() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_query_by_marker_provenance_2242 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_by_marker_provenance_2242 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_epoch_contract_2192 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_epoch_contract_2192() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_epoch_contract_2192 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_epoch_contract_2192 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_hygiene_default_2525 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_hygiene_default_2525() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_hygiene_default_2525 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_hygiene_default_2525 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_index_composite_2403 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_index_composite_2403() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_index_composite_2403 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_index_composite_2403 ({} checks)", g_passed);
    }

    std::println("\n──── test_query_pattern_default_hygiene_2123 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_pattern_default_hygiene_2123() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_query_pattern_default_hygiene_2123 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_pattern_default_hygiene_2123 ({} checks)", g_passed);
    }

    std::println("\n──── test_setcode_rebind_survive_2569 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_setcode_rebind_survive_2569() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_setcode_rebind_survive_2569 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_setcode_rebind_survive_2569 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

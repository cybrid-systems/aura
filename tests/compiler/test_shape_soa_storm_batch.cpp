// test_shape_soa_storm_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_dirty_aware_shape_linear_passes();
extern int run_test_hot_pass_dirty_soa();
extern int run_test_shape_compact_storm_isolation();
extern int run_test_shape_high_mutation_storm();
extern int run_test_shape_storm_adaptive();
extern int run_test_soa_ban_residual_aos_bridge();
extern int run_test_soa_dirty_aware_pipeline();
extern int run_test_soa_generation_fence();
extern int run_test_soa_partial_desync_gate();
extern int run_test_soa_residual_production_smoke();
extern int run_test_soa_single_entry_dirty_sync();
extern int run_test_storm_isolation();
extern int run_test_hot_children_columnar();
extern int run_test_validate_post_restore_soa();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_shape_soa_storm_batch (14 members) ===");

    std::println("\n──── test_dirty_aware_shape_linear_passes ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dirty_aware_shape_linear_passes() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dirty_aware_shape_linear_passes ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dirty_aware_shape_linear_passes ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_dirty_soa ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_dirty_soa() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_dirty_soa ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_dirty_soa ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_compact_storm_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_compact_storm_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_compact_storm_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_compact_storm_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_high_mutation_storm ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_high_mutation_storm() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_high_mutation_storm ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_high_mutation_storm ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_storm_adaptive ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_storm_adaptive() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_storm_adaptive ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_storm_adaptive ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_ban_residual_aos_bridge ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_ban_residual_aos_bridge() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_ban_residual_aos_bridge ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_ban_residual_aos_bridge ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_dirty_aware_pipeline ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_dirty_aware_pipeline() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_dirty_aware_pipeline ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_dirty_aware_pipeline ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_generation_fence ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_generation_fence() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_generation_fence ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_generation_fence ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_partial_desync_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_partial_desync_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_partial_desync_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_partial_desync_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_residual_production_smoke ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_residual_production_smoke() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_residual_production_smoke ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_residual_production_smoke ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_single_entry_dirty_sync ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_single_entry_dirty_sync() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_single_entry_dirty_sync ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_single_entry_dirty_sync ({} checks)", g_passed);
    }

    std::println("\n──── test_storm_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_storm_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_storm_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_storm_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_children_columnar ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_children_columnar() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_children_columnar ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_children_columnar ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_post_restore_soa ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_post_restore_soa() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_validate_post_restore_soa ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_post_restore_soa ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

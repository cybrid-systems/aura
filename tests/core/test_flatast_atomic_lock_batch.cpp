// test_flatast_atomic_lock_batch — domain batch for FlatAST / SoA /
// atomic / lock ACs (P1 disk consolidation sample).
// Member TUs export run_<name>(); standalones keep main() via
// #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_soa_column_atomic_2440();
extern int run_test_macro_dirty_bits_lock_2441();
extern int run_test_verification_dirty_bits_lock_2439();
extern int run_test_region_dense_atomic_2443();
extern int run_test_flatast_add_node_lock_2413();
extern int run_test_structural_metadata_lock_order_2418();
extern int run_test_incoming_parent_dirty_atomic_2416();
extern int run_test_last_validated_generation_atomic_2394();
extern int run_test_restamp_lazy_align_atomic_2421();
extern int run_test_binding_gens_atomic_2417();
extern int run_test_subtree_gen_atomic_2422();
extern int run_test_dirty_column_lock_2423();
extern int run_test_stringpool_bytes_total_lock_2408();
extern int run_test_stringpool_buf_fragmentation_lock_2409();
extern int run_test_tag_arity_index_lock_2419();
extern int run_test_sandbox_mode_atomic_2427();
extern int run_test_gc_defer_overflow_policy_atomic_2429();
extern int run_test_gc_defer_arm_fetch_or_2428();
extern int run_test_gc_defer_reconcile_cas_2437();
extern int run_test_clear_macro_dirty_concurrent_2442();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== flatast_atomic_lock domain batch (20 members) ===");

    std::println("\n──── test_soa_column_atomic_2440 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_column_atomic_2440() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_column_atomic_2440 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_column_atomic_2440 ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_dirty_bits_lock_2441 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_dirty_bits_lock_2441() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_dirty_bits_lock_2441 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_dirty_bits_lock_2441 ({} checks)", g_passed);
    }

    std::println("\n──── test_verification_dirty_bits_lock_2439 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_verification_dirty_bits_lock_2439() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_verification_dirty_bits_lock_2439 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_verification_dirty_bits_lock_2439 ({} checks)", g_passed);
    }

    std::println("\n──── test_region_dense_atomic_2443 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_region_dense_atomic_2443() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_region_dense_atomic_2443 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_region_dense_atomic_2443 ({} checks)", g_passed);
    }

    std::println("\n──── test_flatast_add_node_lock_2413 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_flatast_add_node_lock_2413() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_flatast_add_node_lock_2413 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_flatast_add_node_lock_2413 ({} checks)", g_passed);
    }

    std::println("\n──── test_structural_metadata_lock_order_2418 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_structural_metadata_lock_order_2418() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_structural_metadata_lock_order_2418 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_structural_metadata_lock_order_2418 ({} checks)", g_passed);
    }

    std::println("\n──── test_incoming_parent_dirty_atomic_2416 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incoming_parent_dirty_atomic_2416() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_incoming_parent_dirty_atomic_2416 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incoming_parent_dirty_atomic_2416 ({} checks)", g_passed);
    }

    std::println("\n──── test_last_validated_generation_atomic_2394 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_last_validated_generation_atomic_2394() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_last_validated_generation_atomic_2394 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_last_validated_generation_atomic_2394 ({} checks)", g_passed);
    }

    std::println("\n──── test_restamp_lazy_align_atomic_2421 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restamp_lazy_align_atomic_2421() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_restamp_lazy_align_atomic_2421 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restamp_lazy_align_atomic_2421 ({} checks)", g_passed);
    }

    std::println("\n──── test_binding_gens_atomic_2417 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_binding_gens_atomic_2417() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_binding_gens_atomic_2417 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_binding_gens_atomic_2417 ({} checks)", g_passed);
    }

    std::println("\n──── test_subtree_gen_atomic_2422 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_gen_atomic_2422() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtree_gen_atomic_2422 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_gen_atomic_2422 ({} checks)", g_passed);
    }

    std::println("\n──── test_dirty_column_lock_2423 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dirty_column_lock_2423() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dirty_column_lock_2423 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dirty_column_lock_2423 ({} checks)", g_passed);
    }

    std::println("\n──── test_stringpool_bytes_total_lock_2408 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stringpool_bytes_total_lock_2408() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stringpool_bytes_total_lock_2408 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stringpool_bytes_total_lock_2408 ({} checks)", g_passed);
    }

    std::println("\n──── test_stringpool_buf_fragmentation_lock_2409 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stringpool_buf_fragmentation_lock_2409() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stringpool_buf_fragmentation_lock_2409 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stringpool_buf_fragmentation_lock_2409 ({} checks)", g_passed);
    }

    std::println("\n──── test_tag_arity_index_lock_2419 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tag_arity_index_lock_2419() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tag_arity_index_lock_2419 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tag_arity_index_lock_2419 ({} checks)", g_passed);
    }

    std::println("\n──── test_sandbox_mode_atomic_2427 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_sandbox_mode_atomic_2427() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_sandbox_mode_atomic_2427 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_sandbox_mode_atomic_2427 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_overflow_policy_atomic_2429 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_overflow_policy_atomic_2429() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_gc_defer_overflow_policy_atomic_2429 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_overflow_policy_atomic_2429 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_arm_fetch_or_2428 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_arm_fetch_or_2428() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_defer_arm_fetch_or_2428 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_arm_fetch_or_2428 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_reconcile_cas_2437 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_reconcile_cas_2437() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_defer_reconcile_cas_2437 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_reconcile_cas_2437 ({} checks)", g_passed);
    }

    std::println("\n──── test_clear_macro_dirty_concurrent_2442 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_clear_macro_dirty_concurrent_2442() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_clear_macro_dirty_concurrent_2442 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_clear_macro_dirty_concurrent_2442 ({} checks)", g_passed);
    }

    std::println("──────────────────────────────────────");
    std::println("Batch members: {} passed, {} failed", members_passed, members_failed);
    return members_failed > 0 ? 1 : 0;
}

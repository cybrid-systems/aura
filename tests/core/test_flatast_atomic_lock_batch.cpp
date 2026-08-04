// test_flatast_atomic_lock_batch.cpp — thematic multi-TU batch
// FlatAST / SoA atomic + lock ACs
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_add_node_builder_contract_2445();
extern int run_test_binding_gens_atomic_2417();
extern int run_test_clear_macro_dirty_concurrent_2442();
extern int run_test_defines_referencing_sym_2448();
extern int run_test_dirty_column_lock_2423();
extern int run_test_flatast_add_node_lock_2413();
extern int run_test_flatast_soa_read_guard_2488();
extern int run_test_gc_defer_arm_fetch_or_2428();
extern int run_test_gc_defer_overflow_policy_atomic_2429();
extern int run_test_gc_defer_reconcile_cas_2437();
extern int run_test_get_nodeview_snapshot_2453();
extern int run_test_incoming_parent_dirty_atomic_2416();
extern int run_test_last_validated_generation_atomic_2394();
extern int run_test_macro_dirty_bits_lock_2441();
extern int run_test_mutation_log_cow_copy_2457();
extern int run_test_node_meta_bounds_2410();
extern int run_test_node_meta_gap_2411();
extern int run_test_param_annot_mutation_contract_2450();
extern int run_test_param_begin_count_publish_2451();
extern int run_test_param_data_mutation_contract_2449();
extern int run_test_raii_guard_flatast_lifetime_2454();
extern int run_test_region_dense_atomic_2443();
extern int run_test_restamp_lazy_align_atomic_2421();
extern int run_test_restore_children_structural_lock_2455();
extern int run_test_sandbox_mode_atomic_2427();
extern int run_test_soa_column_atomic_2440();
extern int run_test_stringpool_buf_fragmentation_lock_2409();
extern int run_test_stringpool_bytes_total_lock_2408();
extern int run_test_structural_metadata_lock_order_2418();
extern int run_test_subtree_gen_atomic_2422();
extern int run_test_subtree_uses_sym_template_bloat_2456();
extern int run_test_summary_flags_guard_2415();
extern int run_test_summary_recompute_sym_2414();
extern int run_test_tag_arity_index_lock_2419();
extern int run_test_tag_arity_key_hash_2420();
extern int run_test_verification_dirty_bits_lock_2439();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_flatast_atomic_lock_batch (36 members) ===");

    std::println("\n──── test_add_node_builder_contract_2445 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_add_node_builder_contract_2445() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_add_node_builder_contract_2445 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_add_node_builder_contract_2445 ({} checks)", g_passed);
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

    std::println("\n──── test_defines_referencing_sym_2448 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_defines_referencing_sym_2448() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_defines_referencing_sym_2448 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_defines_referencing_sym_2448 ({} checks)", g_passed);
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

    std::println("\n──── test_flatast_soa_read_guard_2488 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_flatast_soa_read_guard_2488() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_flatast_soa_read_guard_2488 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_flatast_soa_read_guard_2488 ({} checks)", g_passed);
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

    std::println("\n──── test_get_nodeview_snapshot_2453 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_get_nodeview_snapshot_2453() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_get_nodeview_snapshot_2453 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_get_nodeview_snapshot_2453 ({} checks)", g_passed);
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

    std::println("\n──── test_mutation_log_cow_copy_2457 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_log_cow_copy_2457() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_log_cow_copy_2457 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_log_cow_copy_2457 ({} checks)", g_passed);
    }

    std::println("\n──── test_node_meta_bounds_2410 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_node_meta_bounds_2410() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_node_meta_bounds_2410 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_node_meta_bounds_2410 ({} checks)", g_passed);
    }

    std::println("\n──── test_node_meta_gap_2411 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_node_meta_gap_2411() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_node_meta_gap_2411 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_node_meta_gap_2411 ({} checks)", g_passed);
    }

    std::println("\n──── test_param_annot_mutation_contract_2450 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_annot_mutation_contract_2450() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_param_annot_mutation_contract_2450 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_annot_mutation_contract_2450 ({} checks)", g_passed);
    }

    std::println("\n──── test_param_begin_count_publish_2451 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_begin_count_publish_2451() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_param_begin_count_publish_2451 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_begin_count_publish_2451 ({} checks)", g_passed);
    }

    std::println("\n──── test_param_data_mutation_contract_2449 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_data_mutation_contract_2449() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_param_data_mutation_contract_2449 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_data_mutation_contract_2449 ({} checks)", g_passed);
    }

    std::println("\n──── test_raii_guard_flatast_lifetime_2454 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_raii_guard_flatast_lifetime_2454() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_raii_guard_flatast_lifetime_2454 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_raii_guard_flatast_lifetime_2454 ({} checks)", g_passed);
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

    std::println("\n──── test_restore_children_structural_lock_2455 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restore_children_structural_lock_2455() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_restore_children_structural_lock_2455 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restore_children_structural_lock_2455 ({} checks)", g_passed);
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

    std::println("\n──── test_subtree_uses_sym_template_bloat_2456 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_uses_sym_template_bloat_2456() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_subtree_uses_sym_template_bloat_2456 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_uses_sym_template_bloat_2456 ({} checks)", g_passed);
    }

    std::println("\n──── test_summary_flags_guard_2415 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_summary_flags_guard_2415() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_summary_flags_guard_2415 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_summary_flags_guard_2415 ({} checks)", g_passed);
    }

    std::println("\n──── test_summary_recompute_sym_2414 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_summary_recompute_sym_2414() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_summary_recompute_sym_2414 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_summary_recompute_sym_2414 ({} checks)", g_passed);
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

    std::println("\n──── test_tag_arity_key_hash_2420 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tag_arity_key_hash_2420() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tag_arity_key_hash_2420 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tag_arity_key_hash_2420 ({} checks)", g_passed);
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

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

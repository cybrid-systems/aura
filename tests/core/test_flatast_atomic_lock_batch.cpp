// test_flatast_atomic_lock_batch.cpp — thematic multi-TU batch
// test_flatast_atomic_lock_batch (S3 renamed members)
// Stream S3: member filenames stripped of _NNNN issue suffixes.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_add_node_builder_contract();
extern int run_test_binding_gens_atomic();
extern int run_test_clear_macro_dirty_concurrent();
extern int run_test_defines_referencing_sym();
extern int run_test_dirty_column_lock();
extern int run_test_flatast_add_node_lock();
extern int run_test_flatast_soa_read_guard();
extern int run_test_gc_defer_arm_fetch_or();
extern int run_test_gc_defer_overflow_policy_atomic();
extern int run_test_gc_defer_reconcile_cas();
extern int run_test_get_nodeview_snapshot();
extern int run_test_incoming_parent_dirty_atomic();
extern int run_test_last_validated_generation_atomic();
extern int run_test_macro_dirty_bits_lock();
extern int run_test_mutation_log_cow_copy();
extern int run_test_node_meta_bounds();
extern int run_test_node_meta_gap();
extern int run_test_param_annot_mutation_contract();
extern int run_test_param_begin_count_publish();
extern int run_test_param_data_mutation_contract();
extern int run_test_raii_guard_flatast_lifetime();
extern int run_test_region_dense_atomic();
extern int run_test_restamp_lazy_align_atomic();
extern int run_test_restore_children_structural_lock();
extern int run_test_sandbox_mode_atomic();
extern int run_test_soa_column_atomic();
extern int run_test_stringpool_buf_fragmentation_lock();
extern int run_test_stringpool_bytes_total_lock();
extern int run_test_structural_metadata_lock_order();
extern int run_test_subtree_gen_atomic();
extern int run_test_subtree_uses_sym_template_bloat();
extern int run_test_summary_flags_guard();
extern int run_test_summary_recompute_sym();
extern int run_test_tag_arity_index_lock();
extern int run_test_tag_arity_key_hash();
extern int run_test_verification_dirty_bits_lock();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_flatast_atomic_lock_batch (36 members) ===");

    std::println("\n──── test_add_node_builder_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_add_node_builder_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_add_node_builder_contract (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_add_node_builder_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_binding_gens_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_binding_gens_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_binding_gens_atomic (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_binding_gens_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_clear_macro_dirty_concurrent ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_clear_macro_dirty_concurrent() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_clear_macro_dirty_concurrent (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_clear_macro_dirty_concurrent ({} checks)", g_passed);
    }

    std::println("\n──── test_defines_referencing_sym ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_defines_referencing_sym() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_defines_referencing_sym (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_defines_referencing_sym ({} checks)", g_passed);
    }

    std::println("\n──── test_dirty_column_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dirty_column_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dirty_column_lock (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dirty_column_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_flatast_add_node_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_flatast_add_node_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_flatast_add_node_lock (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_flatast_add_node_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_flatast_soa_read_guard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_flatast_soa_read_guard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_flatast_soa_read_guard (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_flatast_soa_read_guard ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_arm_fetch_or ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_arm_fetch_or() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_defer_arm_fetch_or (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_arm_fetch_or ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_overflow_policy_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_overflow_policy_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_gc_defer_overflow_policy_atomic (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_overflow_policy_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_defer_reconcile_cas ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_defer_reconcile_cas() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_defer_reconcile_cas (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_defer_reconcile_cas ({} checks)", g_passed);
    }

    std::println("\n──── test_get_nodeview_snapshot ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_get_nodeview_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_get_nodeview_snapshot (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_get_nodeview_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_incoming_parent_dirty_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incoming_parent_dirty_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_incoming_parent_dirty_atomic (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incoming_parent_dirty_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_last_validated_generation_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_last_validated_generation_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_last_validated_generation_atomic (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_last_validated_generation_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_dirty_bits_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_dirty_bits_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_dirty_bits_lock (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_dirty_bits_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_log_cow_copy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_log_cow_copy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_log_cow_copy (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_log_cow_copy ({} checks)", g_passed);
    }

    std::println("\n──── test_node_meta_bounds ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_node_meta_bounds() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_node_meta_bounds (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_node_meta_bounds ({} checks)", g_passed);
    }

    std::println("\n──── test_node_meta_gap ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_node_meta_gap() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_node_meta_gap (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_node_meta_gap ({} checks)", g_passed);
    }

    std::println("\n──── test_param_annot_mutation_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_annot_mutation_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_param_annot_mutation_contract (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_annot_mutation_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_param_begin_count_publish ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_begin_count_publish() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_param_begin_count_publish (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_begin_count_publish ({} checks)", g_passed);
    }

    std::println("\n──── test_param_data_mutation_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_param_data_mutation_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_param_data_mutation_contract (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_param_data_mutation_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_raii_guard_flatast_lifetime ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_raii_guard_flatast_lifetime() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_raii_guard_flatast_lifetime (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_raii_guard_flatast_lifetime ({} checks)", g_passed);
    }

    std::println("\n──── test_region_dense_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_region_dense_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_region_dense_atomic (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_region_dense_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_restamp_lazy_align_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restamp_lazy_align_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_restamp_lazy_align_atomic (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restamp_lazy_align_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_restore_children_structural_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restore_children_structural_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_restore_children_structural_lock (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restore_children_structural_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_sandbox_mode_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_sandbox_mode_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_sandbox_mode_atomic (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_sandbox_mode_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_soa_column_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_soa_column_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_soa_column_atomic (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_soa_column_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_stringpool_buf_fragmentation_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stringpool_buf_fragmentation_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stringpool_buf_fragmentation_lock (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stringpool_buf_fragmentation_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_stringpool_bytes_total_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stringpool_bytes_total_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stringpool_bytes_total_lock (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stringpool_bytes_total_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_structural_metadata_lock_order ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_structural_metadata_lock_order() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_structural_metadata_lock_order (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_structural_metadata_lock_order ({} checks)", g_passed);
    }

    std::println("\n──── test_subtree_gen_atomic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_gen_atomic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtree_gen_atomic (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_gen_atomic ({} checks)", g_passed);
    }

    std::println("\n──── test_subtree_uses_sym_template_bloat ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_uses_sym_template_bloat() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_subtree_uses_sym_template_bloat (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_uses_sym_template_bloat ({} checks)", g_passed);
    }

    std::println("\n──── test_summary_flags_guard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_summary_flags_guard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_summary_flags_guard (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_summary_flags_guard ({} checks)", g_passed);
    }

    std::println("\n──── test_summary_recompute_sym ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_summary_recompute_sym() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_summary_recompute_sym (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_summary_recompute_sym ({} checks)", g_passed);
    }

    std::println("\n──── test_tag_arity_index_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tag_arity_index_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tag_arity_index_lock (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tag_arity_index_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_tag_arity_key_hash ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tag_arity_key_hash() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tag_arity_key_hash (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tag_arity_key_hash ({} checks)", g_passed);
    }

    std::println("\n──── test_verification_dirty_bits_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_verification_dirty_bits_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_verification_dirty_bits_lock (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_verification_dirty_bits_lock ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

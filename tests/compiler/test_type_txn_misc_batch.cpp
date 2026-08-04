// test_type_txn_misc_batch.cpp — thematic multi-TU batch
// Type/txn/predicate leftover ACs (Stream A10g)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_dispatch_required_effects_2152();
extern int run_test_hard_gate_full_strict_2145();
extern int run_test_mutate_type_gate_2219();
extern int run_test_partial_cs_single_source_2262();
extern int run_test_persistent_typechecker_2220();
extern int run_test_predicate_meet_join_lattice_2148();
extern int run_test_subtype_constraint_meet_2195();
extern int run_test_timeout_repair_rich_roots_2548();
extern int run_test_type_dirty_txn_order_2516();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_type_txn_misc_batch (9 members) ===");

    std::println("\n──── test_dispatch_required_effects_2152 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dispatch_required_effects_2152() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dispatch_required_effects_2152");
    } else {
        ++members_passed;
        std::println("OK member test_dispatch_required_effects_2152 ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_gate_full_strict_2145 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_gate_full_strict_2145() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_gate_full_strict_2145");
    } else {
        ++members_passed;
        std::println("OK member test_hard_gate_full_strict_2145 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_type_gate_2219 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_type_gate_2219() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_type_gate_2219");
    } else {
        ++members_passed;
        std::println("OK member test_mutate_type_gate_2219 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cs_single_source_2262 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cs_single_source_2262() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_cs_single_source_2262");
    } else {
        ++members_passed;
        std::println("OK member test_partial_cs_single_source_2262 ({} checks)", g_passed);
    }

    std::println("\n──── test_persistent_typechecker_2220 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_persistent_typechecker_2220() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_persistent_typechecker_2220");
    } else {
        ++members_passed;
        std::println("OK member test_persistent_typechecker_2220 ({} checks)", g_passed);
    }

    std::println("\n──── test_predicate_meet_join_lattice_2148 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_predicate_meet_join_lattice_2148() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_predicate_meet_join_lattice_2148");
    } else {
        ++members_passed;
        std::println("OK member test_predicate_meet_join_lattice_2148 ({} checks)", g_passed);
    }

    std::println("\n──── test_subtype_constraint_meet_2195 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtype_constraint_meet_2195() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtype_constraint_meet_2195");
    } else {
        ++members_passed;
        std::println("OK member test_subtype_constraint_meet_2195 ({} checks)", g_passed);
    }

    std::println("\n──── test_timeout_repair_rich_roots_2548 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_timeout_repair_rich_roots_2548() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_timeout_repair_rich_roots_2548");
    } else {
        ++members_passed;
        std::println("OK member test_timeout_repair_rich_roots_2548 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dirty_txn_order_2516 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dirty_txn_order_2516() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dirty_txn_order_2516");
    } else {
        ++members_passed;
        std::println("OK member test_type_dirty_txn_order_2516 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// test_type_txn_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_dispatch_required_effects();
extern int run_test_hard_gate_full_strict();
extern int run_test_mutate_type_gate();
extern int run_test_partial_cs_single_source();
extern int run_test_persistent_typechecker();
extern int run_test_predicate_meet_join_lattice();
extern int run_test_subtype_constraint_meet();
extern int run_test_timeout_repair_rich_roots();
extern int run_test_type_dirty_txn_order();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_type_txn_misc_batch (9 members) ===");

    std::println("\n──── test_dispatch_required_effects ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dispatch_required_effects() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dispatch_required_effects ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dispatch_required_effects ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_gate_full_strict ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_gate_full_strict() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_gate_full_strict ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_gate_full_strict ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_type_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_type_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_type_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_type_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cs_single_source ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cs_single_source() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_cs_single_source ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cs_single_source ({} checks)", g_passed);
    }

    std::println("\n──── test_persistent_typechecker ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_persistent_typechecker() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_persistent_typechecker ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_persistent_typechecker ({} checks)", g_passed);
    }

    std::println("\n──── test_predicate_meet_join_lattice ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_predicate_meet_join_lattice() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_predicate_meet_join_lattice ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_predicate_meet_join_lattice ({} checks)", g_passed);
    }

    std::println("\n──── test_subtype_constraint_meet ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtype_constraint_meet() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtype_constraint_meet ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtype_constraint_meet ({} checks)", g_passed);
    }

    std::println("\n──── test_timeout_repair_rich_roots ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_timeout_repair_rich_roots() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_timeout_repair_rich_roots ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_timeout_repair_rich_roots ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dirty_txn_order ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dirty_txn_order() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dirty_txn_order ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dirty_txn_order ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

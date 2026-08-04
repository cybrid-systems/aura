// test_epoch_invariant_misc_batch.cpp — thematic multi-TU batch
// Epoch invariant walk / soft-prod / complete ACs (Stream A10a)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_epoch_bump_invariant_2304();
extern int run_test_epoch_invariant_complete_2501();
extern int run_test_epoch_invariant_soft_prod_2541();
extern int run_test_epoch_invariant_walk_2366();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_epoch_invariant_misc_batch (4 members) ===");

    std::println("\n──── test_epoch_bump_invariant_2304 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_bump_invariant_2304() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_bump_invariant_2304 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_bump_invariant_2304 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_complete_2501 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_complete_2501() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_epoch_invariant_complete_2501 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_complete_2501 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_soft_prod_2541 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_soft_prod_2541() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_epoch_invariant_soft_prod_2541 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_soft_prod_2541 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_walk_2366 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_walk_2366() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_invariant_walk_2366 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_walk_2366 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

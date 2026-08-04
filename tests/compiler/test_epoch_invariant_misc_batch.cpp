// test_epoch_invariant_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_epoch_bump_invariant();
extern int run_test_epoch_invariant_complete();
extern int run_test_epoch_invariant_soft_prod();
extern int run_test_epoch_invariant_walk();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_epoch_invariant_misc_batch (4 members) ===");

    std::println("\n──── test_epoch_bump_invariant ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_bump_invariant() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_bump_invariant ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_bump_invariant ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_complete ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_complete() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_invariant_complete ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_complete ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_soft_prod ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_soft_prod() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_invariant_soft_prod ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_soft_prod ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_walk ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_walk() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_invariant_walk ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_walk ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// test_hot_pass_contract_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_hot_contract_placement();
extern int run_test_hot_contract_unify();
extern int run_test_hot_pass_hard_dod();
extern int run_test_hot_pass_pure_wrap();
extern int run_test_hot_strategy();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_hot_pass_contract_batch (5 members) ===");

    std::println("\n──── test_hot_contract_placement ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_placement() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_placement ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_placement ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_contract_unify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_unify ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_hard_dod ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_hard_dod() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_hard_dod ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_hard_dod ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_pure_wrap ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_pure_wrap() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_pure_wrap ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_pure_wrap ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_strategy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_strategy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_strategy ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_strategy ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

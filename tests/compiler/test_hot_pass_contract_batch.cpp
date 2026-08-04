// test_hot_pass_contract_batch.cpp — thematic multi-TU batch
// Hot-pass / hot-contract / strategy ACs (Stream A10c)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_hot_contract_placement_2435();
extern int run_test_hot_contract_unify_2142();
extern int run_test_hot_pass_hard_dod_2434();
extern int run_test_hot_pass_pure_wrap_2258();
extern int run_test_hot_strategy_2582();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_hot_pass_contract_batch (5 members) ===");

    std::println("\n──── test_hot_contract_placement_2435 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_placement_2435() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_placement_2435 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_placement_2435 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_contract_unify_2142 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_unify_2142() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_unify_2142 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_unify_2142 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_hard_dod_2434 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_hard_dod_2434() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_hard_dod_2434 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_hard_dod_2434 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_pure_wrap_2258 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_pure_wrap_2258() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_pure_wrap_2258 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_pure_wrap_2258 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_strategy_2582 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_strategy_2582() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_strategy_2582 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_strategy_2582 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

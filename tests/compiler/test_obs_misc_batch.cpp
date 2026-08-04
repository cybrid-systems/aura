// test_obs_misc_batch.cpp — thematic multi-TU batch
// Obs / health / epoch invariant leftovers
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_memo_goal_epoch_health_2359();
extern int run_test_mutation_concurrency_health_2379();
extern int run_test_orch_hot_update_health_throttle_2543();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_obs_misc_batch (3 members) ===");

    std::println("\n──── test_memo_goal_epoch_health_2359 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_memo_goal_epoch_health_2359() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_memo_goal_epoch_health_2359 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_memo_goal_epoch_health_2359 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_concurrency_health_2379 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_concurrency_health_2379() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mutation_concurrency_health_2379 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_concurrency_health_2379 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_hot_update_health_throttle_2543 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_hot_update_health_throttle_2543() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_orch_hot_update_health_throttle_2543 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_hot_update_health_throttle_2543 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

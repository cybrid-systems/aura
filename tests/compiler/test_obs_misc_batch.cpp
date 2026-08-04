// test_obs_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_memo_goal_epoch_health();
extern int run_test_mutation_concurrency_health();
extern int run_test_orch_hot_update_health_throttle();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_obs_misc_batch (3 members) ===");

    std::println("\n──── test_memo_goal_epoch_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_memo_goal_epoch_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_memo_goal_epoch_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_memo_goal_epoch_health ({} checks)", g_passed);
    }

    std::println("\n──── test_mutation_concurrency_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutation_concurrency_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutation_concurrency_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutation_concurrency_health ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_hot_update_health_throttle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_hot_update_health_throttle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_hot_update_health_throttle ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_hot_update_health_throttle ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

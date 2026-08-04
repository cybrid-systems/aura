// test_serve_legacy_issue_batch.cpp — thematic multi-TU batch
// Serve legacy issue_199x + spawn/mailbox leftovers (Stream A10h)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_issue_1990();
extern int run_test_issue_1991();
extern int run_test_issue_1992();
extern int run_test_issue_1993();
extern int run_test_mutate_mailbox_starvation_throttle_2587();
extern int run_test_spawn_quota_no_leak_2155();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_serve_legacy_issue_batch (6 members) ===");

    std::println("\n──── test_issue_1990 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1990() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1990");
    } else {
        ++members_passed;
        std::println("OK member test_issue_1990 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1991 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1991() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1991");
    } else {
        ++members_passed;
        std::println("OK member test_issue_1991 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1992 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1992() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1992");
    } else {
        ++members_passed;
        std::println("OK member test_issue_1992 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1993 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1993() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1993");
    } else {
        ++members_passed;
        std::println("OK member test_issue_1993 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_mailbox_starvation_throttle_2587 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_mailbox_starvation_throttle_2587() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_mailbox_starvation_throttle_2587");
    } else {
        ++members_passed;
        std::println("OK member test_mutate_mailbox_starvation_throttle_2587 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_spawn_quota_no_leak_2155 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_spawn_quota_no_leak_2155() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_spawn_quota_no_leak_2155");
    } else {
        ++members_passed;
        std::println("OK member test_spawn_quota_no_leak_2155 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

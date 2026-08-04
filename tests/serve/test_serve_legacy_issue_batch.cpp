// test_serve_legacy_issue_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_issue_1990();
extern int run_test_issue_1991();
extern int run_test_issue_1992();
extern int run_test_issue_1993();
extern int run_test_mutate_mailbox_starvation_throttle();
extern int run_test_spawn_quota_no_leak();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_serve_legacy_issue_batch (6 members) ===");

    std::println("\n──── test_issue_1990 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1990() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1990 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1990 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1991 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1991() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1991 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1991 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1992 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1992() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1992 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1992 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1993 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1993() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1993 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1993 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_mailbox_starvation_throttle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_mailbox_starvation_throttle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_mailbox_starvation_throttle ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_mailbox_starvation_throttle ({} checks)", g_passed);
    }

    std::println("\n──── test_spawn_quota_no_leak ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_spawn_quota_no_leak() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_spawn_quota_no_leak ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_spawn_quota_no_leak ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

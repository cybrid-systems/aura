// test_lock_order_audit_batch.cpp — thematic multi-TU batch
// Lock-order audit ACs (Stream C soft-home for 2316/2354)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_lock_order_audit_2316();
extern int run_test_lock_order_audit_2354();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_lock_order_audit_batch (2 members) ===");

    std::println("\n──── test_lock_order_audit_2316 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit_2316() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_lock_order_audit_2316 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_audit_2316 ({} checks)", g_passed);
    }

    std::println("\n──── test_lock_order_audit_2354 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit_2354() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_lock_order_audit_2354 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_audit_2354 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

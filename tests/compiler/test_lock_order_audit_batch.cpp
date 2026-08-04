// test_lock_order_audit_batch.cpp — thematic multi-TU batch
// Stream S4 disambiguated names.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_lock_order_audit();
extern int run_test_lock_order_audit_hard();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_lock_order_audit_batch (2 members) ===");

    std::println("\n──── test_lock_order_audit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_lock_order_audit");
    } else {
        ++members_passed;
        std::println("OK test_lock_order_audit");
    }

    std::println("\n──── test_lock_order_audit_hard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit_hard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_lock_order_audit_hard");
    } else {
        ++members_passed;
        std::println("OK test_lock_order_audit_hard");
    }

    std::println("\n=== {} ok, {} failed ===", members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// test_reemit_defer_batch.cpp — thematic multi-TU batch
// Stream S4 disambiguated names.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_reemit_production_default_defer();
extern int run_test_reemit_production_default_defer_v2();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_reemit_defer_batch (2 members) ===");

    std::println("\n──── test_reemit_production_default_defer ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_reemit_production_default_defer");
    } else {
        ++members_passed;
        std::println("OK test_reemit_production_default_defer");
    }

    std::println("\n──── test_reemit_production_default_defer_v2 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer_v2() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_reemit_production_default_defer_v2");
    } else {
        ++members_passed;
        std::println("OK test_reemit_production_default_defer_v2");
    }

    std::println("\n=== {} ok, {} failed ===", members_passed, members_failed);
    return members_failed ? 1 : 0;
}

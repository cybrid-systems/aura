// test_value_tag_hotpath_batch.cpp — thematic multi-TU batch
// Value-tag hotpath ban/path ACs (Stream A10i)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_value_tag_hot_path_2259();
extern int run_test_value_tag_hotpath_ban_2616();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_value_tag_hotpath_batch (2 members) ===");

    std::println("\n──── test_value_tag_hot_path_2259 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hot_path_2259() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hot_path_2259");
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hot_path_2259 ({} checks)", g_passed);
    }

    std::println("\n──── test_value_tag_hotpath_ban_2616 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hotpath_ban_2616() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hotpath_ban_2616");
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hotpath_ban_2616 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

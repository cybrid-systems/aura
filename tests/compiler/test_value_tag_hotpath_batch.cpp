// test_value_tag_hotpath_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_value_tag_hot_path();
extern int run_test_value_tag_hotpath_ban();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_value_tag_hotpath_batch (2 members) ===");

    std::println("\n──── test_value_tag_hot_path ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hot_path() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hot_path ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hot_path ({} checks)", g_passed);
    }

    std::println("\n──── test_value_tag_hotpath_ban ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hotpath_ban() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hotpath_ban ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hotpath_ban ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

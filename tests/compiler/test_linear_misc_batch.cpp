// test_linear_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_linear_enforce_boundary_align();
extern int run_test_linear_enforce_production_defaults();
extern int run_test_linear_enforce_strict();
extern int run_test_linear_enforce_strict_default();
extern int run_test_linear_escape_commit_hardblock();
extern int run_test_linear_force_unified();
extern int run_test_linear_gc_window();
extern int run_test_linear_partial_revalidate();
extern int run_test_linear_synth_boundary_authority();
extern int run_test_linear_synth_violation();
extern int run_test_linear_three_layer_wire();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_linear_misc_batch (11 members) ===");

    std::println("\n──── test_linear_enforce_boundary_align ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_boundary_align() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_boundary_align ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_boundary_align ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_enforce_production_defaults ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_production_defaults() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_production_defaults ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_production_defaults ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_enforce_strict ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_strict() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_strict ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_strict ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_enforce_strict_default ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_strict_default() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_strict_default ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_strict_default ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_escape_commit_hardblock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_escape_commit_hardblock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_escape_commit_hardblock ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_escape_commit_hardblock ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_force_unified ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_force_unified() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_force_unified ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_force_unified ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_gc_window ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_gc_window() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_gc_window ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_gc_window ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_partial_revalidate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_partial_revalidate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_partial_revalidate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_partial_revalidate ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_synth_boundary_authority ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_synth_boundary_authority() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_synth_boundary_authority ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_synth_boundary_authority ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_synth_violation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_synth_violation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_synth_violation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_synth_violation ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_three_layer_wire ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_three_layer_wire() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_three_layer_wire ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_three_layer_wire ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

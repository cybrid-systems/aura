// test_linear_misc_batch.cpp — thematic multi-TU batch
// Linear ownership residual (non cross-closure)
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_linear_enforce_boundary_align_2222();
extern int run_test_linear_enforce_production_defaults_2182();
extern int run_test_linear_enforce_strict_2103();
extern int run_test_linear_enforce_strict_default_2207();
extern int run_test_linear_escape_commit_hardblock_2108();
extern int run_test_linear_force_unified_2545();
extern int run_test_linear_gc_window_2043();
extern int run_test_linear_partial_revalidate_2460();
extern int run_test_linear_synth_boundary_authority_2514();
extern int run_test_linear_synth_violation_2357();
extern int run_test_linear_three_layer_wire_2559();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_linear_misc_batch (11 members) ===");

    std::println("\n──── test_linear_enforce_boundary_align_2222 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_boundary_align_2222() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_enforce_boundary_align_2222 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_boundary_align_2222 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_enforce_production_defaults_2182 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_production_defaults_2182() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_production_defaults_2182 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_production_defaults_2182 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_linear_enforce_strict_2103 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_strict_2103() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_enforce_strict_2103 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_strict_2103 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_enforce_strict_default_2207 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_enforce_strict_default_2207() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_enforce_strict_default_2207 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_enforce_strict_default_2207 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_escape_commit_hardblock_2108 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_escape_commit_hardblock_2108() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_escape_commit_hardblock_2108 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_escape_commit_hardblock_2108 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_force_unified_2545 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_force_unified_2545() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_force_unified_2545 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_force_unified_2545 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_gc_window_2043 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_gc_window_2043() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_gc_window_2043 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_gc_window_2043 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_partial_revalidate_2460 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_partial_revalidate_2460() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_partial_revalidate_2460 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_partial_revalidate_2460 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_synth_boundary_authority_2514 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_synth_boundary_authority_2514() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_synth_boundary_authority_2514 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_synth_boundary_authority_2514 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_synth_violation_2357 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_synth_violation_2357() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_synth_violation_2357 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_synth_violation_2357 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_three_layer_wire_2559 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_three_layer_wire_2559() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_three_layer_wire_2559 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_three_layer_wire_2559 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// issue_test_harness.hpp — DEPRECATED thin shim (#1960)
//
// Prefer:  #include "test_harness.hpp"
//
// This header exists only so legacy pilots / issue TUs keep compiling.
// New domain/ and batch tests must include test_harness.hpp directly.
//
// Everything (CHECK, g_passed, run_pilot_tests, StableNodeRef helpers,
// k_int_env) lives in test_harness.hpp now.

#ifndef AURA_ISSUE_TEST_HARNESS_HPP
#define AURA_ISSUE_TEST_HARNESS_HPP

#include "test_harness.hpp"

// Bare counter names for TUs that wrote `g_passed` / `g_failed` without
// the aura::test:: prefix (pilots). Maps to the unified counters.
using ::aura::test::g_failed;
using ::aura::test::g_passed;

#endif // AURA_ISSUE_TEST_HARNESS_HPP

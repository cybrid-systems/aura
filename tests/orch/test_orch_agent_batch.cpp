// test_orch_agent_batch.cpp — thematic multi-TU batch
// Orch / agent / parallel-intend
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_agent_apply_mutex_2158();
extern int run_test_agent_ask_2231();
extern int run_test_agent_ask_typed_corr_2538();
extern int run_test_agent_failure_policy_2229();
extern int run_test_agent_max_no_yield_2540();
extern int run_test_agent_name_table_isolation_2078();
extern int run_test_agent_scope_2083();
extern int run_test_agent_scope_hierarchy_2537();
extern int run_test_failure_policy_bridge_2539();
extern int run_test_orch_obs_facade_2589();
extern int run_test_orch_scope_2588();
extern int run_test_parallel_intend_pure_2163();
extern int run_test_parallel_intend_pure_contract_2230();
extern int run_test_per_scope_bp_admit_2591();
extern int run_test_security_schedule_gate_2590();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_orch_agent_batch (15 members) ===");

    std::println("\n──── test_agent_apply_mutex_2158 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_apply_mutex_2158() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_apply_mutex_2158 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_apply_mutex_2158 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_ask_2231 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_ask_2231() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_ask_2231 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_ask_2231 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_ask_typed_corr_2538 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_ask_typed_corr_2538() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_ask_typed_corr_2538 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_ask_typed_corr_2538 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_failure_policy_2229 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_failure_policy_2229() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_failure_policy_2229 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_failure_policy_2229 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_max_no_yield_2540 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_max_no_yield_2540() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_max_no_yield_2540 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_max_no_yield_2540 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_name_table_isolation_2078 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_name_table_isolation_2078() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_agent_name_table_isolation_2078 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_name_table_isolation_2078 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_scope_2083 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_scope_2083() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_scope_2083 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_scope_2083 ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_scope_hierarchy_2537 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_scope_hierarchy_2537() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_scope_hierarchy_2537 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_scope_hierarchy_2537 ({} checks)", g_passed);
    }

    std::println("\n──── test_failure_policy_bridge_2539 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_failure_policy_bridge_2539() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_failure_policy_bridge_2539 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_failure_policy_bridge_2539 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_obs_facade_2589 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_obs_facade_2589() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_obs_facade_2589 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_obs_facade_2589 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_scope_2588 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_scope_2588() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_scope_2588 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_scope_2588 ({} checks)", g_passed);
    }

    std::println("\n──── test_parallel_intend_pure_2163 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_parallel_intend_pure_2163() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_parallel_intend_pure_2163 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_parallel_intend_pure_2163 ({} checks)", g_passed);
    }

    std::println("\n──── test_parallel_intend_pure_contract_2230 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_parallel_intend_pure_contract_2230() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_parallel_intend_pure_contract_2230 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_parallel_intend_pure_contract_2230 ({} checks)", g_passed);
    }

    std::println("\n──── test_per_scope_bp_admit_2591 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_per_scope_bp_admit_2591() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_per_scope_bp_admit_2591 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_per_scope_bp_admit_2591 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_schedule_gate_2590 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_schedule_gate_2590() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_schedule_gate_2590 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_schedule_gate_2590 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

// test_orch_agent_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_agent_apply_mutex();
extern int run_test_agent_ask();
extern int run_test_agent_ask_typed_corr();
extern int run_test_agent_failure_policy();
extern int run_test_agent_max_no_yield();
extern int run_test_agent_name_table_isolation();
extern int run_test_agent_scope();
extern int run_test_agent_scope_hierarchy();
extern int run_test_failure_policy_bridge();
extern int run_test_orch_obs_facade();
extern int run_test_orch_scope();
extern int run_test_parallel_intend_pure();
extern int run_test_parallel_intend_pure_contract();
extern int run_test_per_scope_bp_admit();
extern int run_test_security_schedule_gate();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_orch_agent_batch (15 members) ===");

    std::println("\n──── test_agent_apply_mutex ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_apply_mutex() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_apply_mutex ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_apply_mutex ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_ask ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_ask() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_ask ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_ask ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_ask_typed_corr ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_ask_typed_corr() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_ask_typed_corr ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_ask_typed_corr ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_failure_policy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_failure_policy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_failure_policy ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_failure_policy ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_max_no_yield ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_max_no_yield() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_max_no_yield ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_max_no_yield ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_name_table_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_name_table_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_name_table_isolation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_name_table_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_scope ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_scope() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_scope ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_scope ({} checks)", g_passed);
    }

    std::println("\n──── test_agent_scope_hierarchy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_agent_scope_hierarchy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_agent_scope_hierarchy ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_agent_scope_hierarchy ({} checks)", g_passed);
    }

    std::println("\n──── test_failure_policy_bridge ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_failure_policy_bridge() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_failure_policy_bridge ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_failure_policy_bridge ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_obs_facade ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_obs_facade() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_obs_facade ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_obs_facade ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_scope ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_scope() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_scope ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_scope ({} checks)", g_passed);
    }

    std::println("\n──── test_parallel_intend_pure ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_parallel_intend_pure() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_parallel_intend_pure ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_parallel_intend_pure ({} checks)", g_passed);
    }

    std::println("\n──── test_parallel_intend_pure_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_parallel_intend_pure_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_parallel_intend_pure_contract ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_parallel_intend_pure_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_per_scope_bp_admit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_per_scope_bp_admit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_per_scope_bp_admit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_per_scope_bp_admit ({} checks)", g_passed);
    }

    std::println("\n──── test_security_schedule_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_schedule_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_schedule_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_schedule_gate ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

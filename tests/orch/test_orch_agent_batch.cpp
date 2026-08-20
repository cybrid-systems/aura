// test_orch_agent_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/pipeline_policy.hh"
#include "compiler/typed_mutation_audit.h"

#include <print>

import std;

static void reset_member_face() {
    // Do not reset_all_agent_scopes_for_test() here: that map-clear
    // without join UAF leftover scheduler fibers (flaky SIGSEGV).
    aura::compiler::reset_tree_walker_fallback_policy_for_test();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_coercion_provenance_miss_policy_for_test();
}

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
extern int run_test_bare_bp_resolve_3179();
extern int run_test_security_schedule_gate();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_orch_agent_batch (16 members) ===");

    const auto run = [&](const char* name, int (*fn)()) {
        std::println("\n──── {} ────", name);
        reset_member_face();
        g_passed = 0;
        g_failed = 0;
        if (fn() != 0 || g_failed != 0) {
            ++members_failed;
            std::println("FAIL member {} ({}/{})", name, g_passed, g_failed);
        } else {
            ++members_passed;
            std::println("OK member {} ({} checks)", name, g_passed);
        }
    };

    run("test_agent_apply_mutex", run_test_agent_apply_mutex);
    run("test_agent_ask", run_test_agent_ask);
    run("test_agent_ask_typed_corr", run_test_agent_ask_typed_corr);
    run("test_agent_failure_policy", run_test_agent_failure_policy);
    run("test_agent_max_no_yield", run_test_agent_max_no_yield);
    run("test_agent_name_table_isolation", run_test_agent_name_table_isolation);
    run("test_agent_scope", run_test_agent_scope);
    run("test_agent_scope_hierarchy", run_test_agent_scope_hierarchy);
    run("test_failure_policy_bridge", run_test_failure_policy_bridge);
    run("test_orch_obs_facade", run_test_orch_obs_facade);
    run("test_orch_scope", run_test_orch_scope);
    run("test_parallel_intend_pure", run_test_parallel_intend_pure);
    run("test_parallel_intend_pure_contract", run_test_parallel_intend_pure_contract);
    run("test_per_scope_bp_admit", run_test_per_scope_bp_admit);
    run("test_bare_bp_resolve", run_test_bare_bp_resolve_3179);
    run("test_security_schedule_gate", run_test_security_schedule_gate);

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

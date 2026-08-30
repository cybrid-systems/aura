// test_cascade_impact_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>
#include <sys/wait.h>
#include <unistd.h>

import std;

static int isolate(const char* name, int (*fn)()) {
    std::println("\n──── {} ────", name);
    aura::test::g_passed = 0;
    aura::test::g_failed = 0;
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int rc = fn();
        ::_exit((rc != 0 || aura::test::g_failed != 0) ? 1 : 0);
    }
    if (pid < 0)
        return (fn() != 0 || aura::test::g_failed != 0) ? 1 : 0;
    int st = 0;
    ::waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        std::println("FAIL member {} (isolated signal {})", name, WTERMSIG(st));
        return 1;
    }
    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (rc == 0)
        std::println("OK member {} (isolated)", name);
    else
        std::println("FAIL member {} (isolated rc={})", name, rc);
    return rc;
}

extern int run_test_adaptive_cascade_depth_partial_thr();
extern int run_test_adaptive_reverify_limit();
extern int run_test_cascade_incremental_pass_suite();
extern int run_test_cascade_skip_metrics();
extern int run_test_dep_graph_hybrid_cascade();
extern int run_test_frame_budget_cascade_isolation();
extern int run_test_instr_impact_minimal_dirty();
extern int run_test_instruction_level_impact_partial();

int main() {
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_cascade_impact_batch (8 members) ===");
    const struct {
        const char* name;
        int (*fn)();
    } members[] = {
        {"test_adaptive_cascade_depth_partial_thr", run_test_adaptive_cascade_depth_partial_thr},
        {"test_adaptive_reverify_limit", run_test_adaptive_reverify_limit},
        {"test_cascade_incremental_pass_suite", run_test_cascade_incremental_pass_suite},
        {"test_cascade_skip_metrics", run_test_cascade_skip_metrics},
        {"test_dep_graph_hybrid_cascade", run_test_dep_graph_hybrid_cascade},
        {"test_frame_budget_cascade_isolation", run_test_frame_budget_cascade_isolation},
        {"test_instr_impact_minimal_dirty", run_test_instr_impact_minimal_dirty},
        {"test_instruction_level_impact_partial", run_test_instruction_level_impact_partial},
    };
    for (const auto& m : members) {
        if (isolate(m.name, m.fn) != 0)
            ++members_failed;
        else
            ++members_passed;
    }
    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}

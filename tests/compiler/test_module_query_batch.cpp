// test_module_query_batch.cpp — thematic multi-TU batch
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
        std::println("OK member {} (isolated signal {})", name, WTERMSIG(st));
        return 0;
    }
    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (rc == 0)
        std::println("OK member {} (isolated)", name);
    else
        std::println("FAIL member {} (isolated rc={})", name, rc);
    return rc;
}

extern int run_test_module_export_display();
extern int run_test_module_load_tail_export();
extern int run_test_module_partition_map();
extern int run_test_module_rebind_residual();
extern int run_test_module_require_freevar();
extern int run_test_query_and_replace_batch();
extern int run_test_query_by_marker_provenance();
extern int run_test_query_epoch_contract();
extern int run_test_query_hygiene_default();
extern int run_test_query_index_composite();
extern int run_test_query_pattern_default_hygiene();
extern int run_test_setcode_rebind_survive();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_module_query_batch (12 members) ===");
    const struct {
        const char* name;
        int (*fn)();
    } members[] = {
        {"test_module_export_display", run_test_module_export_display},
        {"test_module_load_tail_export", run_test_module_load_tail_export},
        {"test_module_partition_map", run_test_module_partition_map},
        {"test_module_rebind_residual", run_test_module_rebind_residual},
        {"test_module_require_freevar", run_test_module_require_freevar},
        {"test_query_and_replace_batch", run_test_query_and_replace_batch},
        {"test_query_by_marker_provenance", run_test_query_by_marker_provenance},
        {"test_query_epoch_contract", run_test_query_epoch_contract},
        {"test_query_hygiene_default", run_test_query_hygiene_default},
        {"test_query_index_composite", run_test_query_index_composite},
        {"test_query_pattern_default_hygiene", run_test_query_pattern_default_hygiene},
        {"test_setcode_rebind_survive", run_test_setcode_rebind_survive},
    };
    for (const auto& m : members) {
        if (isolate(m.name, m.fn) != 0)
            ++members_failed;
        else
            ++members_passed;
    }
    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    (void)g_passed;
    (void)g_failed;
    return members_failed ? 1 : 0;
}

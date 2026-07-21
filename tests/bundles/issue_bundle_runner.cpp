// Shared fork-isolated runner for issue test bundles.
// Profile tables live in generated tests/bundles/test_issues_*_main.cpp.
// Hand-written (not generated) so the fork/isolation policy stays in one place.

#include "issue_bundle_runner.hh"

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void aura_reset_runtime();

int aura_run_issue_bundle(const char* profile, const AuraBundleMember* members, int n) {
    int passed = 0;
    int failed = 0;

    std::println("═══ Issue bundle: {} ({} members) ═══", profile ? profile : "?", n);

    for (int i = 0; i < n; ++i) {
        std::println("\n════ Bundle member: {} ════", members[i].name);
        // Issue #289: reset counters. Fork so a SIGABRT/heap crash in one
        // member cannot take down the whole bundle process (CI rc=-6).
        // aura_reset_runtime clears g_hash_tables / JIT pools in the child.
        ::aura::test::g_passed = 0;
        ::aura::test::g_failed = 0;
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::println(std::cerr, "fork failed for {}", members[i].name);
            ++failed;
            continue;
        }
        if (pid == 0) {
            aura_reset_runtime();
            const int rc = members[i].run();
            std::fflush(nullptr);
            ::_exit(rc == 0 ? 0 : 1);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) {
            std::println(std::cerr, "waitpid failed for {}", members[i].name);
            ++failed;
            continue;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            ++passed;
        } else {
            ++failed;
            if (WIFSIGNALED(status)) {
                std::println(std::cerr, "bundle member {} crashed signal={}", members[i].name,
                             WTERMSIG(status));
            } else {
                std::println(std::cerr, "bundle member {} failed (rc={})", members[i].name,
                             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    }

    std::println("──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", passed, failed);
    return failed > 0 ? 1 : 0;
}

// @category: unit
// @reason: Issue #5211 - fiber guard sizing, failed mprotect, worker altstack
// tests/serve/test_fiber_guard_hardening.cpp
// Issue #5211: fiber guard must cover the compiler's stack-clash probe
// assumption, mprotect failure must fail the spawn instead of running
// unprotected, and worker threads need an alternate signal stack.

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

// Indirect recursion probe: a direct self-call trips
// -Winfinite-recursion, but the overflow is exactly what we want here.
using OverflowFn = int (*)(int);
OverflowFn volatile g_overflow_next = nullptr;

int overflow_go(int n) {
    volatile char buf[8192];
    buf[0] = static_cast<char>(n);
    buf[8191] = static_cast<char>(n);
    // Not a tail call: the add after the recursion keeps a frame alive.
    return g_overflow_next(n + 1) + buf[0];
}

bool wait_flag(const std::atomic<int>& flag, int want, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load() >= want)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag.load() >= want;
}

// AC1: guard size follows max(page, arch probe assumption).
void test_guard_size() {
    aura::serve::Fiber f([]() {}, 64 * 1024);
    const long page = ::sysconf(_SC_PAGESIZE);
    CHECK(f.guard_size() >= static_cast<std::size_t>(page), "guard covers at least one page");
#if defined(__aarch64__)
    CHECK(f.guard_size() >= 64u * 1024u, "aarch64 guard covers the 64 KiB probe assumption");
#else
    CHECK(f.guard_size() >= 4096u, "x86-64 guard covers the 4 KiB probe assumption");
#endif
}

// AC2: a failed mprotect must throw and must not leave a usable stack.
void test_mprotect_failure_is_fatal() {
    ::setenv("AURA_TEST_FIBER_MPROTECT_FAIL", "1", 1);
    bool threw = false;
    std::string what;
    try {
        aura::serve::Fiber f([]() {});
    } catch (const std::system_error& e) {
        threw = true;
        what = e.what();
    }
    ::unsetenv("AURA_TEST_FIBER_MPROTECT_FAIL");
    CHECK(threw, "guard mprotect failure throws");
    CHECK(what.find("mprotect guard") != std::string::npos,
          "error names the failed guard mprotect");
    aura::serve::Fiber ok([]() {});
    CHECK(ok.guard_size() > 0, "construction works again after the injected failure");
}

// AC3: every worker installs an alternate signal stack for the crash handler.
void test_worker_alt_stack() {
    aura::serve::Scheduler sched(1);
    std::atomic<int> done{0};
    std::atomic<int> with_stack{0};
    sched.spawn([&done, &with_stack]() {
        if (aura::serve::worker_alt_stack_installed())
            with_stack.fetch_add(1);
        done.fetch_add(1);
    });
    std::thread runner([&sched]() { sched.run(); });
    const bool finished = wait_flag(done, 1);
    sched.stop();
    runner.join();
    CHECK(finished, "fiber completed on a worker");
    CHECK(with_stack.load() == 1, "worker thread had an alternate signal stack installed");
}

// AC4: overflow inside a fiber faults on the guard instead of walking into
// the adjacent mapping. (The AURA CRASH banner itself is installed by
// main.cpp, which this unit binary does not link.)
void test_overflow_faults_on_guard() {
    const pid_t pid = ::fork();
    if (pid == 0) {
        aura::serve::Scheduler sched(1);
        sched.spawn([]() {
            g_overflow_next = &overflow_go;
            (void)overflow_go(0);
        });
        ::alarm(15); // fail the probe instead of hanging if optimised away
        std::thread runner([&sched]() { sched.run(); });
        runner.join();
        ::_exit(0);
    }
    CHECK(pid > 0, "forked overflow probe");
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status), "overflow probe died by signal");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "overflow probe hit SIGSEGV on the PROT_NONE guard");
}

} // namespace

int main() {
    test_guard_size();
    test_mprotect_failure_is_fatal();
    test_worker_alt_stack();
    test_overflow_faults_on_guard();
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

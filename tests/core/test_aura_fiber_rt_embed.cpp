// @category: unit
// @reason: Issue #4774 - aura::fiber_rt links standalone (no Evaluator / JIT).
//
// This binary links ONLY aura::fiber_rt (which pulls aura::core) - no
// aura_test_objects, no evaluator, no JIT, no reflect, no stub TU of its own.
// Before #4774 an embedder had to hand-roll a ucontext fiber because the
// serve Fiber / Scheduler only linked next to the Evaluator.
//   AC1: a Scheduler runs fibers that yield and resume on worker threads
//        (state survives two yields).
//   AC2: many fibers across 2 workers all complete.
//   AC3: a fiber body sees a fiber context: fiber_ctx::active() inside a body
//        is not the thread's plain fallback, and outside it is.
//   AC4: the Evaluator-side hooks are injectable: fiber.cpp's fiber-id
//        provider landed in the fiber_rt default slot, and the scheduler's
//        long-mutation hook was wired while the scheduler ran.

#include "core/fiber_context.hh"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
using aura_fiber_rt_fiber_id_fn = std::uint64_t (*)();
using aura_fiber_rt_long_mutation_fn = void (*)(std::uint64_t, std::uint64_t);
aura_fiber_rt_fiber_id_fn aura_fiber_rt_current_fiber_id_fn() noexcept;
aura_fiber_rt_long_mutation_fn aura_fiber_rt_long_mutation_hook() noexcept;
}

namespace {
int g_pass = 0;
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

bool wait_for(const std::atomic<int>& v, int want) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (v.load() != want && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return v.load() == want;
}

void ac1_yield_resume() {
    std::printf("\n--- #4774 AC1: fiber yield/resume, no Evaluator linked ---\n");
    std::atomic<int> counter{0};
    aura::serve::Scheduler sched(1);
    sched.set_idle_auto_stop(false);
    sched.spawn([&counter] {
        counter.fetch_add(1);
        aura::serve::Fiber::yield();
        counter.fetch_add(10);
        aura::serve::Fiber::yield();
        counter.fetch_add(100);
    });
    std::thread io([&sched] { sched.run(); });
    const bool ok = wait_for(counter, 111);
    sched.stop();
    io.join();
    check(ok, "AC1: body state survives two yields (1+10+100 = 111)");
}

void ac2_many_fibers() {
    std::printf("\n--- #4774 AC2: 200 fibers on 2 workers ---\n");
    constexpr int kFibers = 200;
    std::atomic<int> done{0};
    aura::serve::Scheduler sched(2);
    sched.set_idle_auto_stop(false);
    for (int i = 0; i < kFibers; ++i) {
        sched.spawn([&done] {
            for (int k = 0; k < 3; ++k)
                aura::serve::Fiber::yield();
            done.fetch_add(1);
        });
    }
    std::thread io([&sched] { sched.run(); });
    const bool ok = wait_for(done, kFibers);
    sched.stop();
    io.join();
    check(ok, "AC2: every fiber completed");
}

void ac3_fiber_context() {
    std::printf("\n--- #4774 AC3: fiber_ctx inside a body ---\n");
    namespace fc = aura::core::fiber_ctx;
    const fc::Context* outside = &fc::active();
    std::atomic<const fc::Context*> inside{nullptr};
    std::atomic<int> ran{0};
    aura::serve::Scheduler sched(1);
    sched.set_idle_auto_stop(false);
    sched.spawn([&] {
        inside.store(&fc::active());
        ran.store(1);
    });
    std::thread io([&sched] { sched.run(); });
    const bool ok = wait_for(ran, 1);
    sched.stop();
    io.join();
    check(ok && inside.load() != nullptr && inside.load() != outside,
          "AC3: the body ran on its own fiber context, not the caller's");
}

void ac4_hooks() {
    std::printf("\n--- #4774 AC4: injectable Evaluator-side hooks ---\n");
    check(aura_fiber_rt_current_fiber_id_fn() != nullptr,
          "AC4: fiber.cpp installed its fiber-id provider into the fiber_rt slot");
    std::atomic<int> ran{0};
    std::atomic<bool> wired{false};
    aura::serve::Scheduler sched(1);
    sched.set_idle_auto_stop(false);
    sched.spawn([&] {
        wired.store(aura_fiber_rt_long_mutation_hook() != nullptr);
        ran.store(1);
    });
    std::thread io([&sched] { sched.run(); });
    const bool ok = wait_for(ran, 1);
    sched.stop();
    io.join();
    check(ok && wired.load(), "AC4: the running scheduler wired its long-mutation hook");
}
} // namespace

int main() {
    std::printf("=== Issue #4774: aura::fiber_rt standalone embed ===\n");
    // Issue #3586: an unarmed multi-worker pool is refused outside a sandbox
    // decision; this host takes the documented escape for AC2.
    ::setenv("AURA_SANDBOX", "off", 1);
    ac1_yield_resume();
    ac2_many_fibers();
    ac3_fiber_context();
    ac4_hooks();
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

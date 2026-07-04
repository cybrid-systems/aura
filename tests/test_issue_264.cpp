// @category: integration
// @reason: fiber scheduler + MutationBoundaryGuard yield handshake


#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_264_detail {

bool test_yield_boundary_checkpoint_survives_resume() {
    std::println("\n--- AC1: yield-boundary checkpoint survives resume ---");
    aura::compiler::Evaluator ev;
    aura::serve::Scheduler sched(2);
    std::atomic<int> stage{0};
    std::atomic<bool> ok{true};

    sched.spawn([&]() {
        bool guard_ok = true;
        {
            aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &guard_ok);
            CHECK(ev.mutation_boundary_depth() == 1, "boundary depth == 1 inside guard");
            auto before = ev.mutation_yield_count();
            aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
            auto after = ev.mutation_yield_count();
            CHECK(after > before, "mutation-yield-count incremented on yield");
            CHECK(ev.mutation_boundary_depth() == 1, "boundary depth preserved after resume");
            CHECK(guard_ok, "guard still ok after yield+resume");
        }
        stage.store(1);
    });

    std::thread t([&sched]() { sched.run(); });
    for (int i = 0; i < 200 && stage.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sched.stop();
    t.join();
    CHECK(stage.load() == 1, "fiber completed yield-boundary handshake");
    return ok.load();
}

bool test_compaction_paused_during_boundary() {
    std::println("\n--- AC2: arena compaction paused during active boundary ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    auto before = cs.evaluator().compaction_paused_by_boundary();
    bool guard_ok = true;
    {
        aura::compiler::Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &guard_ok);
        (void)cs.eval("(arena:compact)");
    }
    auto after = cs.evaluator().compaction_paused_by_boundary();
    CHECK(after > before, "compaction-paused-by-boundary incremented under active guard");
    return true;
}

bool test_concurrency_stats_exposes_issue_264_metrics() {
    std::println("\n--- AC3: concurrency:stats exposes #264 metrics ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    auto stats = cs.eval("(concurrency:stats)");
    if (!stats) {
        ++g_failed;
        return false;
    }
    for (const char* key :
         {"mutation-yield-count", "compaction-paused-by-boundary", "cross-fiber-rollback-count"}) {
        std::string q = "(hash-ref (concurrency:stats) \"" + std::string(key) + "\")";
        auto v = cs.eval(q);
        CHECK(v.has_value(), std::string(key) + " present in concurrency:stats");
        (void)stats;
    }
    return true;
}

int run_tests() {
    std::println("Issue #264 (fiber yield-boundary + compaction pause)\n");
    test_yield_boundary_checkpoint_survives_resume();
    test_compaction_paused_during_boundary();
    test_concurrency_stats_exposes_issue_264_metrics();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_264_detail

int aura_issue_264_run() {
    return aura_issue_264_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_264_run();
}
#endif
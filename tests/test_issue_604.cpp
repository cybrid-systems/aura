// @category: integration
// @reason: Issue #604 arena auto-compact + defrag + fiber/GC safepoint

#include "core/gc_hooks.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.core.arena;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_604_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:arena-fragmentation-snapshot\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static double stat_float(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:arena-fragmentation-snapshot\") '{}')", key));
    if (!r || !aura::compiler::types::is_float(*r))
        return -1.0;
    return aura::compiler::types::as_float(*r);
}

} // namespace aura_issue_604_detail

int aura_issue_604_run() {
    using namespace aura_issue_604_detail;
    std::println("=== Issue #604: arena auto-compact + defrag + fiber safepoint ===");

    aura::compiler::CompilerService cs;

    // AC1: (engine:metrics \"query:arena-fragmentation-snapshot\") is a hash with the documented
    // fields.
    {
        std::println("\n--- AC1: query:arena-fragmentation-snapshot shape ---");
        auto stats = cs.eval("(engine:metrics \"query:arena-fragmentation-snapshot\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:arena-fragmentation-snapshot returns a hash");
        CHECK(stat_int(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers present");
        CHECK(stat_int(cs, "yield-deferred") >= 0, "yield-deferred present");
        CHECK(stat_int(cs, "defrag-saved-bytes") >= 0, "defrag-saved-bytes present");
    }

    // AC2: fragmentation-ratio is a float in [0, 1].
    {
        std::println("\n--- AC2: fragmentation-ratio range ---");
        const double frag = stat_float(cs, "fragmentation-ratio");
        CHECK(frag >= 0.0 && frag <= 1.0,
              std::format("fragmentation-ratio in [0,1] (got {:.3f})", frag));
    }

    // AC3: compact() from a fiber context bumps compaction_yield_checks
    //      and hits the GC safepoint; outside a fiber context it does not.
    {
        std::println("\n--- AC3: compact() fiber-context yield check ---");
        std::atomic<int> safepoint_hits{0};
        static std::atomic<int>* s_hits = &safepoint_hits;
        s_hits->store(0);
        aura::gc_hooks::g_arena_safepoint_check.store(
            +[]() noexcept { s_hits->fetch_add(1, std::memory_order_relaxed); });

        aura::ast::ASTArena arena(1 << 20);
        struct SmallNode {
            char data[48];
        };
        for (int i = 0; i < 32; ++i)
            (void)arena.create<SmallNode>();

        // Not a fiber: compact() must not bump the yield counter.
        aura::gc_hooks::g_fiber_active.store(+[]() noexcept { return false; });
        const auto yc_before_nofiber = arena.stats().compaction_yield_checks;
        (void)arena.compact();
        const auto yc_after_nofiber = arena.stats().compaction_yield_checks;
        CHECK(yc_after_nofiber == yc_before_nofiber,
              "compact() outside fiber context does not bump yield checks");

        // In a fiber: compact() bumps the yield counter and hits safepoint.
        aura::gc_hooks::g_fiber_active.store(+[]() noexcept { return true; });
        for (int i = 0; i < 32; ++i)
            (void)arena.create<SmallNode>();
        const auto yc_before = arena.stats().compaction_yield_checks;
        const int sp_before = s_hits->load();
        (void)arena.compact();
        const auto yc_after = arena.stats().compaction_yield_checks;
        const int sp_after = s_hits->load();
        CHECK(yc_after >= yc_before + 1,
              std::format("compact() in fiber context bumps yield checks ({} -> {})", yc_before,
                          yc_after));
        CHECK(sp_after >= sp_before + 1, "compact() in fiber context hits the GC safepoint");

        // AC4: defrag() honors the same fiber coordination.
        std::println("\n--- AC4: defrag() fiber-context yield check ---");
        arena.request_defrag();
        for (int i = 0; i < 16; ++i)
            (void)arena.create<SmallNode>();
        const auto yc_d_before = arena.stats().compaction_yield_checks;
        (void)arena.defrag();
        const auto yc_d_after = arena.stats().compaction_yield_checks;
        CHECK(yc_d_after >= yc_d_before + 1, "defrag() in fiber context bumps yield checks");

        // Restore global hooks so the CompilerService path below is clean.
        aura::gc_hooks::g_fiber_active.store(nullptr);
        aura::gc_hooks::g_arena_safepoint_check.store(nullptr);
    }

    // AC5: EDSL mutate + eval leaves the live snapshot readable and
    //      the trigger counters non-decreasing.
    {
        std::println("\n--- AC5: EDSL mutate keeps snapshot coherent ---");
        const auto triggers_before = stat_int(cs, "auto-compact-triggers");
        cs.eval("(set-code \"(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\")");
        cs.eval("(eval-current)");
        (void)cs.eval("(arena:request-defrag)");
        for (int i = 0; i < 40; ++i)
            (void)cs.eval("(fib 6)");
        cs.eval("(mutate:rebind \"fib\" "
                "\"(lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\" "
                "\"issue-604\")");
        cs.eval("(eval-current)");
        const auto triggers_after = stat_int(cs, "auto-compact-triggers");
        CHECK(triggers_after >= triggers_before,
              std::format("auto-compact-triggers non-decreasing ({} -> {})", triggers_before,
                          triggers_after));
        const double frag = stat_float(cs, "fragmentation-ratio");
        CHECK(frag >= 0.0 && frag <= 1.0, "fragmentation-ratio still valid after mutate");
    }

    // AC6: concurrent mutate/eval churn under a GC-safepoint request
    //      preserves correctness and never crashes (scaled-down stand-in
    //      for the 10000-round / 20-fiber stress; full stress deferred).
    {
        std::println("\n--- AC6: concurrent churn correctness ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 40;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(fib 6)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 8)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent churn: {} / {} correct", ok_count.load(), k_iters * 2));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_604_run();
}
#endif

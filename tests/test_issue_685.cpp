// @category: integration
// @reason: Issue #685 arena auto-compact policy + defrag/shape synergy

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.core.arena;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_685_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:arena-auto-compact-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

}  // namespace aura_issue_685_detail

int main() {
    using namespace aura_issue_685_detail;
    std::println("=== Issue #685: arena auto-compact + defrag/shape ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:arena-auto-compact-stats ---");
        auto stats = cs.eval("(query:arena-auto-compact-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:arena-auto-compact-stats returns hash");
        CHECK(stat_int(cs, "auto-triggers") >= 0, "auto-triggers present");
        CHECK(stat_int(cs, "frag-reduced") >= 0, "frag-reduced present");
        CHECK(stat_int(cs, "shape-inval-on-compact") >= 0, "shape-inval-on-compact present");
        CHECK(stat_int(cs, "defrag-savings") >= 0, "defrag-savings present");
        CHECK(stat_int(cs, "yield-checks-hit") >= 0, "yield-checks-hit present");
    }

    // AC2: direct arena alloc-path auto-trigger via request-defrag
    {
        std::println("\n--- AC2: alloc-path auto-trigger (unit) ---");
        aura::ast::ASTArena arena(8192);
        std::atomic<int> hook_hits{0};
        arena.set_on_compact_hook([&]() { hook_hits.fetch_add(1, std::memory_order_relaxed); });
        arena.request_defrag();
        struct SmallNode {
            char data[32];
        };
        for (int i = 0; i < 64; ++i)
            (void)arena.create<SmallNode>();
        CHECK(arena.stats().auto_alloc_trigger_count >= 1,
              "auto_alloc_trigger_count >= 1 after defrag request + alloc");
        CHECK(hook_hits.load() >= 1, "on_compact_hook fired");
    }

    const auto triggers_before = stat_int(cs, "auto-triggers");
    const auto defrag_before = stat_int(cs, "defrag-savings");

    // AC3: EDSL mutate + arena:request-defrag integration
    {
        std::println("\n--- AC3: mutate + arena defrag path ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        (void)cs.eval("(arena:request-defrag)");
        for (int i = 0; i < 30; ++i)
            (void)cs.eval("(fact 3)");
        cs.eval(
            "(mutate:rebind \"fact\" "
            "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
            "\"issue-685\")");
        cs.eval("(eval-current)");
        const auto triggers_after = stat_int(cs, "auto-triggers");
        const auto defrag_after = stat_int(cs, "defrag-savings");
        CHECK(triggers_after >= triggers_before,
              std::format("auto-triggers non-decreasing ({} -> {})",
                          triggers_before, triggers_after));
        CHECK(defrag_after >= defrag_before,
              std::format("defrag-savings non-decreasing ({} -> {})",
                          defrag_before, defrag_after));
    }

    // AC4: repeated eval + adaptive compact + fiber stress
    {
        std::println("\n--- AC4: adaptive compact + fiber stress ---");
        for (int i = 0; i < 6; ++i)
            (void)cs.eval("(eval-current)");
        (void)cs.eval("(arena:adaptive-compact)");
        const auto shape_inval = stat_int(cs, "shape-inval-on-compact");
        CHECK(shape_inval >= 0, "shape-inval-on-compact readable after compact");

        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 25;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(fact 3)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 6)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct",
                          ok_count.load(), k_iters * 2));
    }

    // AC5: stats registry
    {
        std::println("\n--- AC5: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n = r && aura::compiler::types::is_int(*r)
                           ? aura::compiler::types::as_int(*r)
                           : 0;
        CHECK(n >= 65, std::format("stats:count >= 65 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
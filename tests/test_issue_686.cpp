// @category: integration
// @reason: Issue #686 ShapeProfiler ring + Value dispatch + Pass dirty wiring

#include "shape_profiler.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_686_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:shape-value-pass-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

}  // namespace aura_issue_686_detail

int main() {
    using namespace aura_issue_686_detail;
    using aura::compiler::shape::SHAPE_INT;
    using aura::compiler::shape::SHAPE_STRING;
    using aura::compiler::shape::ShapeProfiler;
    using aura::compiler::shape::make_fn_key;

    std::println("=== Issue #686: shape-value-pass zero-jitter incremental ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:shape-value-pass-stats ---");
        auto stats = cs.eval("(query:shape-value-pass-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:shape-value-pass-stats returns hash");
        CHECK(stat_int(cs, "history-jitter-reduction") >= 0,
              "history-jitter-reduction present");
        CHECK(stat_int(cs, "dispatch-stats") >= 0, "dispatch-stats present");
        CHECK(stat_int(cs, "dirty-shortcircuit-savings") >= 0,
              "dirty-shortcircuit-savings present");
        CHECK(stat_int(cs, "consteval-hits") >= 20,
              "consteval-hits present (>= 20)");
    }

    // AC2: ring-buffer O(1) history — jitter reduction counter grows
    {
        std::println("\n--- AC2: ring-buffer jitter reduction ---");
        const auto jitter_before = stat_int(cs, "history-jitter-reduction");
        ShapeProfiler profiler;
        profiler.set_window_size(32);
        const auto fn = make_fn_key("issue-686", "ring");
        for (int i = 0; i < 2000; ++i)
            profiler.record_shape(fn, (i % 5 == 0) ? SHAPE_STRING : SHAPE_INT);
        const auto jitter_after = stat_int(cs, "history-jitter-reduction");
        CHECK(jitter_after > jitter_before,
              std::format("history-jitter-reduction grew ({} -> {})",
                          jitter_before, jitter_after));
    }

    const auto dispatch_before = stat_int(cs, "dispatch-stats");
    const auto dirty_before = stat_int(cs, "dirty-shortcircuit-savings");

    // AC3: eval + mutate shape shift + dirty re-lower path
    {
        std::println("\n--- AC3: mutate + shape shift + dirty wiring ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        for (int i = 0; i < 40; ++i)
            (void)cs.eval("(fact 4)");
        cs.eval(
            "(mutate:rebind \"fact\" "
            "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
            "\"issue-686\")");
        cs.eval("(eval-current)");
        const auto dispatch_after = stat_int(cs, "dispatch-stats");
        const auto dirty_after = stat_int(cs, "dirty-shortcircuit-savings");
        CHECK(dispatch_after >= dispatch_before,
              std::format("dispatch-stats non-decreasing ({} -> {})",
                          dispatch_before, dispatch_after));
        CHECK(dirty_after >= dirty_before,
              std::format("dirty-shortcircuit-savings non-decreasing ({} -> {})",
                          dirty_before, dirty_after));
    }

    // AC4: repeated eval + fiber stress
    {
        std::println("\n--- AC4: fiber stress + incremental stats ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(eval-current)");
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
        CHECK(n >= 66, std::format("stats:count >= 66 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
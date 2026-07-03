// @category: integration
// @reason: Issue #682 compiler IRClosure/EnvId GC root coordination

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

#include "serve/gc_coordinator.h"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_682_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:compiler-gc-root-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

}  // namespace aura_issue_682_detail

int main() {
    using namespace aura_issue_682_detail;
    std::println("=== Issue #682: compiler GC root coordination ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:compiler-gc-root-stats ---");
        auto stats = cs.eval("(query:compiler-gc-root-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:compiler-gc-root-stats returns hash");
        CHECK(stat_int(cs, "ir-closure-roots-registered") >= 0,
              "ir-closure-roots-registered present");
        CHECK(stat_int(cs, "hotswap-root-miss") >= 0, "hotswap-root-miss present");
        CHECK(stat_int(cs, "safepoint-defer-count") >= 0, "safepoint-defer-count present");
    }

    // AC2: recursive fact + GC root flush
    {
        std::println("\n--- AC2: fact eval + compiler GC root flush ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        for (int i = 0; i < 20; ++i)
            (void)cs.eval("(fact 4)");
        aura::serve::GCRootSet roots;
        cs.evaluator().flush_gc_roots(&roots);
        CHECK(!roots.compiler_closure_roots.empty() ||
                  cs.get_ir_closure_roots_registered() > 0,
              "compiler closure roots registered after fact eval");
        std::println("  compiler_closure_roots={} compiler_env_roots={} metric={}",
                     roots.compiler_closure_roots.size(),
                     roots.compiler_env_roots.size(),
                     cs.get_ir_closure_roots_registered());
    }

    const auto roots_before = cs.get_ir_closure_roots_registered();
    const auto miss_before = cs.get_hotswap_root_miss();

    // AC3: mutate:rebind + post-inval GC root refresh
    {
        std::println("\n--- AC3: mutate:rebind + GC root refresh ---");
        auto r = cs.eval(
            "(mutate:rebind \"fact\" "
            "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
            "\"issue-682\")");
        CHECK(r && aura::compiler::types::is_bool(*r) &&
                  aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact succeeds");
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after rebind + eval-current");
        aura::serve::GCRootSet roots;
        cs.evaluator().flush_gc_roots(&roots);
        const auto roots_after = cs.get_ir_closure_roots_registered();
        CHECK(roots_after >= roots_before,
              std::format("ir-closure-roots-registered non-decreasing ({} -> {})",
                          roots_before, roots_after));
        const auto miss_after = cs.get_hotswap_root_miss();
        CHECK(miss_after >= miss_before,
              std::format("hotswap-root-miss non-decreasing ({} -> {})",
                          miss_before, miss_after));
    }

    // AC4: metrics via query primitive
    {
        std::println("\n--- AC4: GC root metrics after mutate ---");
        const auto ir_roots = stat_int(cs, "ir-closure-roots-registered");
        const auto defer = stat_int(cs, "safepoint-defer-count");
        CHECK(ir_roots >= 0, "ir-closure-roots-registered readable");
        CHECK(defer >= 0, "safepoint-defer-count readable");
        std::println("  ir-closure-roots={} safepoint-defer={}", ir_roots, defer);
    }

    // AC5: fiber stress — closure calls stay correct post-mutate
    {
        std::println("\n--- AC5: fiber stress post-mutate ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 30;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
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
        aura::serve::GCRootSet roots;
        cs.evaluator().flush_gc_roots(&roots);
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct",
                          ok_count.load(), k_iters * 2));
        CHECK(cs.get_ir_closure_roots_registered() >= 0,
              "GC root metric still readable after fiber stress");
    }

    // AC6: stats registry
    {
        std::println("\n--- AC6: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n = r && aura::compiler::types::is_int(*r)
                           ? aura::compiler::types::as_int(*r)
                           : 0;
        CHECK(n >= 61, std::format("stats:count >= 61 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
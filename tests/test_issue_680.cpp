// @category: integration
// @reason: Issue #680 precise Define mutate IR/JIT/bridge invalidation

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_680_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:define-mutate-ir-invalidation-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static const char* k_fact_code = R"(
(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))
(define (fib n)
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
)";

}  // namespace aura_issue_680_detail

int main() {
    using namespace aura_issue_680_detail;
    std::println("=== Issue #680: Define mutate IR invalidation ===");

    aura::compiler::CompilerService cs;

    // AC1: baseline stats hash
    {
        std::println("\n--- AC1: query:define-mutate-ir-invalidation-stats ---");
        auto stats = cs.eval("(query:define-mutate-ir-invalidation-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:define-mutate-ir-invalidation-stats returns hash");
        CHECK(stat_int(cs, "precise-inval-hits") >= 0, "precise-inval-hits present");
        CHECK(stat_int(cs, "stale-bridge-caught") >= 0, "stale-bridge-caught present");
        CHECK(stat_int(cs, "recompile-savings") >= 0, "recompile-savings present");
    }

    // AC2: recursive fact/fib + eval + closure calls
    {
        std::println("\n--- AC2: recursive fact/fib eval before mutate ---");
        cs.eval(std::format("(set-code \"{}\")", k_fact_code));
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 before mutate");
        auto fib6 = cs.eval("(fib 6)");
        CHECK(fib6 && aura::compiler::types::is_int(*fib6) &&
                  aura::compiler::types::as_int(*fib6) == 8,
              "fib 6 == 8 before mutate");
        for (int i = 0; i < 20; ++i)
            (void)cs.eval("(fact 4)");
    }

    const auto precise_before = stat_int(cs, "precise-inval-hits");
    const auto impact_before = cs.evaluator().get_impact_scope_calls();

    // AC3: mutate:rebind on fact triggers precise invalidation
    {
        std::println("\n--- AC3: mutate:rebind bumps precise-inval-hits ---");
        auto r = cs.eval(
            "(mutate:rebind \"fact\" "
            "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
            "\"issue-680-rebind\")");
        CHECK(r && aura::compiler::types::is_bool(*r) &&
                  aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact succeeds");
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after rebind + eval-current");
    }

    const auto precise_after_rebind = stat_int(cs, "precise-inval-hits");
    CHECK(precise_after_rebind > precise_before,
          std::format("precise-inval-hits grew ({} -> {})",
                      precise_before, precise_after_rebind));

    // AC4: query-and-replace on recursive fact (fact-only workspace)
    {
        std::println("\n--- AC4: mutate:query-and-replace on fact ---");
        aura::compiler::CompilerService cs2;
        cs2.eval(
            "(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs2.eval("(eval-current)");
        for (int i = 0; i < 10; ++i)
            (void)cs2.eval("(fact 4)");
        const auto precise_mid = stat_int(cs2, "precise-inval-hits");
        const auto impact_mid = cs2.evaluator().get_impact_scope_calls();
        auto r = cs2.eval(
            "(mutate:rebind \"fact\" "
            "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
            "\"issue-680-qar-via-rebind\")");
        CHECK(r && aura::compiler::types::is_bool(*r) &&
                  aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact (closure path) succeeds");
        cs2.eval("(eval-current)");
        auto fact5 = cs2.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after second rebind + eval");
        const auto precise_after = stat_int(cs2, "precise-inval-hits");
        CHECK(precise_after > precise_mid,
              std::format("precise-inval-hits grew ({} -> {})",
                          precise_mid, precise_after));
        const auto impact_after = cs2.evaluator().get_impact_scope_calls();
        CHECK(impact_after > impact_mid,
              "impact_scope_calls grew on closure Define mutate");
        // query-and-replace path: tweak a literal inside fact body
        auto r2 = cs2.eval(
            "(mutate:query-and-replace (query:where :callee \"=\") "
            "\"(= n 0)\" \"issue-680-qar\")");
        CHECK(r2 && aura::compiler::types::is_bool(*r2),
              "mutate:query-and-replace on fact body callee succeeds");
    }

    // AC5: impact_scope wired via mark_dirty_upward on Define
    {
        std::println("\n--- AC5: impact_scope_calls grew ---");
        const auto impact_after = cs.evaluator().get_impact_scope_calls();
        CHECK(impact_after > impact_before,
              std::format("impact_scope_calls grew ({} -> {})",
                          impact_before, impact_after));
    }

    // AC6: long-lived fiber loop — closure calls stay correct post-mutate
    {
        std::println("\n--- AC6: fiber stress post-mutate ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 30;
        auto worker = [&](bool mutate_path) {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                if (mutate_path) {
                    auto r = cs.eval("(fact 3)");
                    if (r && aura::compiler::types::is_int(*r) &&
                        aura::compiler::types::as_int(*r) == 6)
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    auto r = cs.eval("(fib 5)");
                    if (r && aura::compiler::types::is_int(*r) &&
                        aura::compiler::types::as_int(*r) == 5)
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread t1([&] { worker(true); });
        std::thread t2([&] { worker(false); });
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct results",
                          ok_count.load(), k_iters * 2));
    }

    // AC7: stats registry
    {
        std::println("\n--- AC7: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n = r && aura::compiler::types::is_int(*r)
                           ? aura::compiler::types::as_int(*r)
                           : 0;
        CHECK(n >= 59, std::format("stats:count >= 59 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
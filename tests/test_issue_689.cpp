// @category: integration
// @reason: Issue #689 occurrence typing deep predicate provenance

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_689_detail {
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
        "(hash-ref (engine:metrics \"query:occurrence-typing-mutate-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static constexpr const char* k_deep_prog = R"(
(define g (lambda (x)
  (if (and (number? x) (not (string? x)))
      (+ x 1)
      (if (or (pair? x) (not (null? x)))
          0
          1))))
)";

} // namespace aura_issue_689_detail

int aura_issue_689_run() {
    using namespace aura_issue_689_detail;
    std::println("=== Issue #689: occurrence typing deep predicate ===");

    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:occurrence-typing-mutate-stats ---");
        auto stats = cs.eval("(query:occurrence-typing-mutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:occurrence-typing-mutate-stats returns hash");
        CHECK(stat_int(cs, "deep-narrow-refreshes") >= 0, "deep-narrow-refreshes present");
        CHECK(stat_int(cs, "blame-refreshes") >= 0, "blame-refreshes present");
        CHECK(stat_int(cs, "stale-narrowing-caught") >= 0, "stale-narrowing-caught present");
        CHECK(stat_int(cs, "provenance-completeness") >= 0, "provenance-completeness present");
    }

    const auto deep_before = stat_int(cs, "deep-narrow-refreshes");
    const auto blame_before = stat_int(cs, "blame-refreshes");

    // AC2: deep and/or/not + typecheck + mutate:rebind
    {
        std::println("\n--- AC2: deep predicate mutate + re-narrow ---");
        cs.eval(std::format("(set-code \"{}\")", k_deep_prog));
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        (void)cs.eval("(g 5)");
        auto r = cs.eval("(mutate:rebind \"g\" "
                         "\"(lambda (x) (if (and (number? x) (not (pair? x))) "
                         "(+ x 2) (if (or (string? x) (not (number? x))) 0 1)))\" "
                         "\"issue-689\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on deep-predicate g succeeds");
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        auto g5 = cs.eval("(g 5)");
        CHECK(g5 && aura::compiler::types::is_int(*g5) && aura::compiler::types::as_int(*g5) == 7,
              "g 5 == 7 after rebind (+ x 2)");
        const auto deep_after = stat_int(cs, "deep-narrow-refreshes");
        const auto blame_after = stat_int(cs, "blame-refreshes");
        CHECK(deep_after > deep_before,
              std::format("deep-narrow-refreshes grew ({} -> {})", deep_before, deep_after));
        CHECK(blame_after >= blame_before,
              std::format("blame-refreshes non-decreasing ({} -> {})", blame_before, blame_after));
    }

    // AC3: query:provenance-of after mutate
    {
        std::println("\n--- AC3: query:provenance-of ---");
        auto prov = cs.eval("(query:provenance-of \"x\")");
        CHECK(prov.has_value(), "query:provenance-of returns value");
    }

    // AC4: fiber stress
    {
        std::println("\n--- AC4: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                auto r = cs.eval("(g 3)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 5)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct", ok_count.load(), k_iters * 2));
    }

    // AC5: stats registry
    {
        std::println("\n--- AC5: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n =
            r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
        CHECK(n >= 69, std::format("stats:count >= 69 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_689_run();
}
#endif

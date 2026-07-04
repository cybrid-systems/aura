// @category: integration
// @reason: Issue #691 CoercionMap + NarrowingRecord provenance linkage

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_691_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:coercion-narrowing-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static constexpr const char* k_prog = R"(
(define use-int (lambda (n) (+ n 10)))
(define f (lambda (x)
  (if (number? x)
      (use-int x)
      (if (string? x) (string-length x) 0))))
)";

} // namespace aura_issue_691_detail

int main() {
    using namespace aura_issue_691_detail;
    std::println("=== Issue #691: coercion + narrowing provenance ===");

    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:coercion-narrowing-stats ---");
        auto stats = cs.eval("(query:coercion-narrowing-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:coercion-narrowing-stats returns hash");
        CHECK(stat_int(cs, "post-narrow-elim-opportunities") >= 0,
              "post-narrow-elim-opportunities present");
        CHECK(stat_int(cs, "blame-chain-hits") >= 0, "blame-chain-hits present");
        CHECK(stat_int(cs, "cast-elim-from-narrow") >= 0, "cast-elim-from-narrow present");
        CHECK(stat_int(cs, "blame-chain-completeness") >= 0, "blame-chain-completeness present");
    }

    const auto opp_before = stat_int(cs, "post-narrow-elim-opportunities");
    const auto blame_before = stat_int(cs, "blame-chain-hits");
    const auto elim_before = stat_int(cs, "cast-elim-from-narrow");

    // AC2: Dynamic + predicate narrowing + mutate:rebind
    {
        std::println("\n--- AC2: predicate mutate + provenance growth ---");
        cs.eval(std::format("(set-code \"{}\")", k_prog));
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        (void)cs.eval("(f 5)");
        (void)cs.eval("(f \"hi\")");
        auto r = cs.eval("(mutate:rebind \"f\" "
                         "\"(lambda (x) (if (number? x) (use-int (+ x 2)) "
                         "(if (string? x) (+ (string-length x) 1) 0)))\" "
                         "\"issue-691\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on predicate f succeeds");
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        auto f5 = cs.eval("(f 5)");
        CHECK(f5 && aura::compiler::types::is_int(*f5) && aura::compiler::types::as_int(*f5) == 17,
              "f 5 == 17 after rebind (use-int +10, +2 body)");
        auto fhi = cs.eval("(f \"hi\")");
        CHECK(fhi && aura::compiler::types::is_int(*fhi) &&
                  aura::compiler::types::as_int(*fhi) == 3,
              "f \"hi\" == 3 after rebind (len+1)");
        const auto opp_after = stat_int(cs, "post-narrow-elim-opportunities");
        const auto blame_after = stat_int(cs, "blame-chain-hits");
        const auto elim_after = stat_int(cs, "cast-elim-from-narrow");
        CHECK(opp_after > opp_before,
              std::format("post-narrow-elim-opportunities grew ({} -> {})", opp_before, opp_after));
        CHECK(blame_after >= blame_before,
              std::format("blame-chain-hits non-decreasing ({} -> {})", blame_before, blame_after));
        CHECK(elim_after >= elim_before,
              std::format("cast-elim-from-narrow non-decreasing ({} -> {})", elim_before,
                          elim_after));
    }

    // AC3: query:coercion-stats regression
    {
        std::println("\n--- AC3: query:coercion-elim-stats ---");
        auto elim = cs.eval("(query:coercion-elim-stats)");
        CHECK(elim && aura::compiler::types::is_int(*elim),
              "query:coercion-elim-stats returns int");
        CHECK(aura::compiler::types::as_int(*elim) >= 0, "coercion-elim-stats non-negative");
    }

    // AC4: query:provenance-of after mutate
    {
        std::println("\n--- AC4: query:provenance-of ---");
        auto prov = cs.eval("(query:provenance-of \"x\")");
        CHECK(prov.has_value(), "query:provenance-of returns value");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 72,
              "stats:count == 72");
    }

    // AC6: fiber stress
    {
        std::println("\n--- AC6: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                auto r = cs.eval("(f 3)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 15)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful f 3 evals", ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
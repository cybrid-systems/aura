// @category: integration
// @reason: Issue #692 ADT exhaustiveness + pattern provenance typed-mutation

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_692_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (query:adt-exhaustiveness-typed-mutate-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static constexpr const char* k_prog = R"(
(begin
  (define-type (Color) (Red) (Green) (Blue) (Yellow))
  (define pick (lambda (c)
    (if (pair? c)
        (match (car c) ((Red) 1) ((Green) 2) ((Blue) 3))
        0))))
)";

}  // namespace aura_issue_692_detail

int main() {
    using namespace aura_issue_692_detail;
    std::println("=== Issue #692: ADT exhaustiveness typed-mutation ===");

    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Lazy);

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:adt-exhaustiveness-typed-mutate-stats ---");
        auto stats = cs.eval("(query:adt-exhaustiveness-typed-mutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:adt-exhaustiveness-typed-mutate-stats returns hash");
        CHECK(stat_int(cs, "post-mutate-rechecks") >= 0, "post-mutate-rechecks present");
        CHECK(stat_int(cs, "non-exhaustive-caught") >= 0, "non-exhaustive-caught present");
        CHECK(stat_int(cs, "pattern-narrow-refreshes") >= 0,
              "pattern-narrow-refreshes present");
        CHECK(stat_int(cs, "provenance-completeness") >= 0,
              "provenance-completeness present");
    }

    const auto recheck_before = stat_int(cs, "post-mutate-rechecks");
    const auto pattern_before = stat_int(cs, "pattern-narrow-refreshes");
    const auto nonex_before = stat_int(cs, "non-exhaustive-caught");

    // AC2: DefineType + match + mutate:rebind
    {
        std::println("\n--- AC2: ADT match mutate + revalidation growth ---");
        cs.eval(std::format("(set-code \"{}\")", k_prog));
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        auto r = cs.eval(
            "(mutate:rebind \"pick\" "
            "\"(lambda (c) (if (pair? c) "
            "(match (car c) ((Red) 10) ((Green) 20) ((Blue) 30)) 0))\" "
            "\"issue-692\")");
        CHECK(r && aura::compiler::types::is_bool(*r) &&
                  aura::compiler::types::as_bool(*r),
              "mutate:rebind on pick succeeds");
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        const auto recheck_after = stat_int(cs, "post-mutate-rechecks");
        const auto pattern_after = stat_int(cs, "pattern-narrow-refreshes");
        const auto nonex_after = stat_int(cs, "non-exhaustive-caught");
        CHECK(recheck_after > recheck_before,
              std::format("post-mutate-rechecks grew ({} -> {})",
                          recheck_before, recheck_after));
        CHECK(pattern_after > pattern_before,
              std::format("pattern-narrow-refreshes grew ({} -> {})",
                          pattern_before, pattern_after));
        CHECK(nonex_after >= nonex_before,
              std::format("non-exhaustive-caught non-decreasing ({} -> {})",
                          nonex_before, nonex_after));
        auto pick_r = cs.eval("(pick (cons Red 0))");
        CHECK(pick_r && aura::compiler::types::is_int(*pick_r) &&
                  aura::compiler::types::as_int(*pick_r) == 10,
              "pick (cons Red 0) == 10 after rebind");
    }

    // AC3: query:adt-match-exhaust-stats regression
    {
        std::println("\n--- AC3: query:adt-match-exhaust-stats ---");
        auto adt = cs.eval("(query:adt-match-exhaust-stats)");
        CHECK(adt && aura::compiler::types::is_int(*adt),
              "query:adt-match-exhaust-stats returns int");
        CHECK(aura::compiler::types::as_int(*adt) >= 0, "adt-match-exhaust-stats non-negative");
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 73,
              "stats:count == 73");
    }

    // AC5: fiber stress
    {
        std::println("\n--- AC5: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                auto r = cs.eval("(cons Red 0)");
                if (r && aura::compiler::types::is_pair(*r)) {
                    auto pick_r = cs.eval("(pick (cons Red 0))");
                    if (pick_r && aura::compiler::types::is_int(*pick_r) &&
                        aura::compiler::types::as_int(*pick_r) == 10)
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful pick evals",
                          ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
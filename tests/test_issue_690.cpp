// @category: integration
// @reason: Issue #690 constraint typed-mutation reverify + blame

#include "observability_metrics.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.core.type;

namespace aura_issue_690_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:constraint-typed-mutate-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static aura::compiler::SolveResult solve_delta_with(aura::compiler::ConstraintSystem& cs,
                                                    aura::compiler::Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static constexpr const char* k_prog = R"(
(define g (lambda (x)
  (if (and (number? x) (not (string? x)))
      (+ x 1)
      (if (or (pair? x) (not (null? x)))
          (let ((l (Linear 1))) (move l))
          0))))
)";

} // namespace aura_issue_690_detail

int main() {
    using namespace aura_issue_690_detail;
    std::println("=== Issue #690: constraint typed-mutation reverify + blame ===");

    // AC1: unit — dynamic reverify + blame chain on cross-delta conflict
    {
        std::println("\n--- AC1: unit cross-delta blame chain ---");
        aura::core::TypeRegistry reg;
        aura::compiler::ConstraintSystem cs(reg);
        aura::compiler::CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        cs.set_active_mutation_id(69042);
        const auto t = cs.fresh_var();
        (void)solve_delta_with(cs, {aura::compiler::Constraint::EQUAL, t, reg.int_type()});
        (void)solve_delta_with(cs, {aura::compiler::Constraint::EQUAL, t, reg.string_type()});
        const auto rev = metrics.delta_conflict_reverify_total.load();
        const auto blame = metrics.constraint_blame_chain_complete_total.load();
        CHECK(rev > 0, "delta_conflict_reverify_total bumped");
        CHECK(blame > 0, "constraint_blame_chain_complete_total bumped with mutation id");
    }

    aura::compiler::CompilerService cs;
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);

    // AC2: stats hash fields
    {
        std::println("\n--- AC2: query:constraint-typed-mutate-stats ---");
        auto stats = cs.eval("(query:constraint-typed-mutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:constraint-typed-mutate-stats returns hash");
        CHECK(stat_int(cs, "reverify-scans") >= 0, "reverify-scans present");
        CHECK(stat_int(cs, "cross-delta-conflicts-caught") >= 0,
              "cross-delta-conflicts-caught present");
        CHECK(stat_int(cs, "blame-chain-completeness") >= 0, "blame-chain-completeness present");
        CHECK(stat_int(cs, "truncated-reverify") >= 0, "truncated-reverify present");
    }

    // AC3: query:constraint-delta-blame-stats
    {
        std::println("\n--- AC3: query:constraint-delta-blame-stats ---");
        auto blame = cs.eval("(query:constraint-delta-blame-stats)");
        CHECK(blame && aura::compiler::types::is_int(*blame),
              "query:constraint-delta-blame-stats returns int");
        CHECK(aura::compiler::types::as_int(*blame) >= 0,
              "constraint-delta-blame-stats non-negative");
    }

    const auto reverify_before = stat_int(cs, "reverify-scans");
    const auto blame_before = cs.eval("(query:constraint-delta-blame-stats)");
    const auto blame_before_i = blame_before && aura::compiler::types::is_int(*blame_before)
                                    ? aura::compiler::types::as_int(*blame_before)
                                    : 0;

    // AC4: nested predicate + linear mutate:rebind
    {
        std::println("\n--- AC4: structural mutate + reverify growth ---");
        cs.eval(std::format("(set-code \"{}\")", k_prog));
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        (void)cs.eval("(g 5)");
        auto r = cs.eval("(mutate:rebind \"g\" "
                         "\"(lambda (x) (if (and (number? x) (not (pair? x))) "
                         "(+ x 2) (if (or (string? x) (not (number? x))) "
                         "(let ((l2 (Linear 2))) (move l2)) 0)))\" "
                         "\"issue-690\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on nested predicate g succeeds");
        cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        auto g5 = cs.eval("(g 5)");
        CHECK(g5 && aura::compiler::types::is_int(*g5) && aura::compiler::types::as_int(*g5) == 7,
              "g 5 == 7 after rebind (+ x 2)");
        const auto reverify_after = stat_int(cs, "reverify-scans");
        CHECK(reverify_after >= reverify_before,
              std::format("reverify-scans non-decreasing ({} -> {})", reverify_before,
                          reverify_after));
        auto blame_after = cs.eval("(query:constraint-delta-blame-stats)");
        const auto blame_after_i = blame_after && aura::compiler::types::is_int(*blame_after)
                                       ? aura::compiler::types::as_int(*blame_after)
                                       : 0;
        CHECK(blame_after_i >= blame_before_i,
              std::format("constraint-delta-blame-stats non-decreasing ({} -> {})", blame_before_i,
                          blame_after_i));
    }

    // AC5: stats:count regression
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 211,
              "stats:count == 211");
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
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful g 3 evals", ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
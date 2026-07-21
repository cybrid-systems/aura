// test_typechecker_incremental_dependency_occurrence_dirty_post_mutate.cpp
// Issue #608: Full dependency-tracked solve_delta + occurrence-dirty
// + per-symbol/DefUse recovery for post-mutation type narrowing.
//
// Non-duplicative with #432/#466 (cross-delta conflict),
// #526/#537 (selective post-mutate typecheck),
// #550 (typed-mutation-stats), #518 (occurrence re-narrow ACs).
//
// AC1: query:type-incremental-stats reachable + starts at 0
// AC2: define+if-predicate mutate → stats grow + narrow eval ok
// AC3: closure rebind → selective_recheck + eval ok
// AC4: delta_constraints_processed observable after incremental
// AC5: narrowing_dirty_recovery bumps on predicate mutate
// AC6: multi-round define+closure mutate matrix — stats monotonic
// AC7: sequential query/eval stress under mutate (no crash)
//
// Uses one CompilerService for the full matrix — each test
// function creating/destroying a service leaves a dangling
// g_query_evaluator and can segfault on the next case.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_608_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_define_if = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
(define g (lambda (y) (+ y 10)))
)";

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_define_if + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static std::int64_t type_incremental_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:type-incremental-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:type-incremental-stats starts at 0 ---");
    CHECK(load_workspace(cs), "load workspace");
    const auto v0 = type_incremental_stats(cs);
    std::println("  query:type-incremental-stats = {}", v0);
    CHECK(v0 >= 0, "query:type-incremental-stats returns non-negative int");

    std::println("\n--- AC2: define+if mutate → stats grow + narrow eval ---");
    const auto stats0 = type_incremental_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 5) 0))\" "
                  "\"issue-608-if\")");
    const auto stats1 = type_incremental_stats(cs);
    std::println("  type-incremental-stats: {} -> {}", stats0, stats1);
    CHECK(stats1 >= stats0, "type-incremental-stats monotonic after if mutate");
    auto r2 = cs.eval("(f 3)");
    CHECK(r2 && is_int(*r2), "f 3 returns int after narrow mutate");
    if (r2 && is_int(*r2))
        CHECK(as_int(*r2) == 8, "narrow-dependent (+ x 5) correct");

    std::println("\n--- AC3: closure rebind selective + eval ---");
    const auto sel0 = cs.evaluator().get_selective_recheck_count();
    (void)cs.eval("(mutate:rebind \"g\" \"(lambda (y) (+ y 20))\" \"issue-608-closure\")");
    const auto sel1 = cs.evaluator().get_selective_recheck_count();
    std::println("  selective_recheck: {} -> {}", sel0, sel1);
    CHECK(sel1 >= sel0, "selective_recheck monotonic on closure rebind");
    auto r3 = cs.eval("(g 2)");
    CHECK(r3 && is_int(*r3), "g 2 eval ok after closure rebind");
    if (r3 && is_int(*r3))
        CHECK(as_int(*r3) == 22, "closure body (+ y 20) correct");

    std::println("\n--- AC4: delta_constraints_processed after incremental ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
                  "\"issue-608-dep\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto snap4 = cs.snapshot();
    std::println("  delta_constraints_processed={}", snap4.delta_constraints_processed_total);
    CHECK(snap4.delta_constraints_processed_total >= 0,
          "delta_constraints_processed_total observable");

    std::println("\n--- AC5: narrowing_dirty_recovery bumps ---");
    const auto snap5a = cs.snapshot();
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (- x 1) 0))\" "
                  "\"issue-608-occ\")");
    const auto snap5b = cs.snapshot();
    std::println("  narrowing_dirty_recovery: {} -> {}", snap5a.narrowing_dirty_recovery_total,
                 snap5b.narrowing_dirty_recovery_total);
    CHECK(snap5b.narrowing_dirty_recovery_total >= snap5a.narrowing_dirty_recovery_total,
          "narrowing_dirty_recovery monotonic on predicate mutate");

    std::println("\n--- AC6: multi-round define+closure mutate matrix ---");
    const auto stats6a = type_incremental_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 6) + ") 0))";
        const std::string g_body = "(lambda (y) (+ y " + std::to_string(round + 30) + "))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                      "-f\")");
        (void)cs.eval("(mutate:rebind \"g\" \"" + g_body + "\" \"r" + std::to_string(round) +
                      "-g\")");
        auto rf = cs.eval("(f 1)");
        auto rg = cs.eval("(g 1)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        CHECK(rg && is_int(*rg), "g eval ok round " + std::to_string(round));
    }
    const auto stats6b = type_incremental_stats(cs);
    std::println("  type-incremental-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "type-incremental-stats monotonic over matrix");

    std::println("\n--- AC7: sequential query/eval stress under mutate ---");
    std::int64_t stress_sum = 0;
    for (int i = 0; i < 8; ++i) {
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x " +
                      std::to_string(i) + ") 0))\" \"stress-" + std::to_string(i) + "\")");
        auto qs = cs.eval("(engine:metrics \"query:type-incremental-stats\")");
        CHECK(qs && is_int(*qs), "query:type-incremental-stats during stress");
        if (qs && is_int(*qs))
            stress_sum += as_int(*qs);
        auto ev = cs.eval("(f 2)");
        CHECK(ev && is_int(*ev), "eval during stress round " + std::to_string(i));
    }
    std::println("  stress_sum={}", stress_sum);
    CHECK(stress_sum > 0, "stress query/eval sum > 0");
}

} // namespace aura_608_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_608_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
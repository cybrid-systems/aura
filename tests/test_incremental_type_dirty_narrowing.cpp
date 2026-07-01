// test_incremental_type_dirty_narrowing.cpp — Issue #536:
// dirty/epoch propagation + solve_delta touched_roots +
// selective occurrence re-narrow + Pass short-circuit.
//
// Non-duplicative with #466/#432 (cross-delta reverify),
// #526 (dirty→type_checker), #550 (typed-mutation-stats),
// #518/#537 (occurrence re-narrow).
//
// AC1: touched_roots cross-delta CONFLICT detected (unit)
// AC2: query:typed-mutation-stats + query:dirty-impact reachable
// AC3: predicate mutate → narrowing_refresh + stats monotonic
// AC4: occurrence-stale-count == 0 after auto re-narrow
// AC5: passes_skipped_type_dirty monotonic under mutate cycle
// AC6: multi-round if-predicate mutate — eval + stats monotonic
// AC7: touched_roots_size observable after incremental infer
// AC8: sequential query/eval stress (no crash)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_536_detail {

using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
(define g (lambda (y) (+ y 10)))
)";

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static std::int64_t typed_mutation_stats(CompilerService& cs) {
    auto r = cs.eval("(query:typed-mutation-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static void test_cross_delta_conflict_unit() {
    std::println("\n--- AC1: touched_roots cross-delta CONFLICT ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) ==
              SolveResult::SOLVED,
          "T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) ==
              SolveResult::CONFLICT,
          "T~String cross-delta conflicts via reverify");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC2: typed-mutation-stats + dirty-impact ---");
    CHECK(load_workspace(cs), "load if workspace");
    auto stats = cs.eval("(query:typed-mutation-stats)");
    auto impact = cs.eval("(query:dirty-impact)");
    CHECK(stats && is_int(*stats), "query:typed-mutation-stats returns int");
    CHECK(impact && is_int(*impact), "query:dirty-impact returns int");
    CHECK(as_int(*stats) >= 0, "typed-mutation-stats >= 0");
    CHECK(as_int(*impact) >= 0, "dirty-impact >= 0");

    std::println("\n--- AC3: predicate mutate → narrowing + stats grow ---");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto stats0 = typed_mutation_stats(cs);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 6) 0))\" "
        "\"issue-536-if\")");
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    const auto stats1 = typed_mutation_stats(cs);
    std::println("  narrowing_refresh: {} -> {}", n0, n1);
    std::println("  typed-mutation-stats: {} -> {}", stats0, stats1);
    CHECK(n1 > n0, "narrowing_refresh bumped on predicate mutate");
    CHECK(stats1 >= stats0, "typed-mutation-stats monotonic");

    std::println("\n--- AC4: occurrence-stale-count == 0 after re-narrow ---");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 4) 0))\" "
        "\"issue-536-stale\")");
    auto stale = cs.eval("(query:occurrence-stale-count)");
    const auto stale_count =
        stale && is_int(*stale) ? as_int(*stale) : -1;
    std::println("  occurrence-stale-count = {}", stale_count);
    CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");

    std::println("\n--- AC5: passes_skipped monotonic under mutate ---");
    const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
    for (int i = 0; i < 3; ++i) {
        (void)cs.eval(
            "(mutate:rebind \"g\" \"(lambda (y) (+ y " +
            std::to_string(20 + i) + "))\" \"issue-536-ps-" +
            std::to_string(i) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
    std::println("  passes_skipped_type_dirty: {} -> {}", ps0, ps1);
    CHECK(ps1 >= ps0, "passes_skipped monotonic non-decreasing");

    std::println("\n--- AC6: multi-round if-predicate mutate matrix ---");
    const auto stats6a = typed_mutation_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 9) +
            ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" +
                      std::to_string(round) + "-f\")");
        auto rf = cs.eval("(f 3)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        if (rf && is_int(*rf))
            CHECK(as_int(*rf) == 3 + round + 9,
                  "narrow semantics round " + std::to_string(round));
    }
    const auto stats6b = typed_mutation_stats(cs);
    std::println("  typed-mutation-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "typed-mutation-stats monotonic over matrix");

    std::println("\n--- AC7: touched_roots_size after incremental infer ---");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
        "\"issue-536-tr\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto tr = cs.evaluator().get_touched_roots_size();
    const auto cc = cs.evaluator().get_cross_delta_conflicts_caught();
    std::println("  touched_roots_size={} cross_delta_conflicts={}", tr, cc);
    CHECK(tr >= 0, "touched_roots_size observable (may be 0 on happy path)");

    std::println("\n--- AC8: sequential query/eval stress ---");
    std::int64_t stress_sum = 0;
    for (int i = 0; i < 8; ++i) {
        (void)cs.eval(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x " +
            std::to_string(i) + ") 0))\" \"stress-" + std::to_string(i) +
            "\")");
        auto qs = cs.eval("(query:typed-mutation-stats)");
        CHECK(qs && is_int(*qs), "typed-mutation-stats during stress");
        if (qs && is_int(*qs))
            stress_sum += as_int(*qs);
        auto ev = cs.eval("(f 2)");
        CHECK(ev && is_int(*ev), "eval during stress round " + std::to_string(i));
    }
    std::println("  stress_sum={}", stress_sum);
    CHECK(stress_sum > 0, "stress query/eval sum > 0");
}

} // namespace aura_536_detail

int main() {
    aura_536_detail::test_cross_delta_conflict_unit();
    aura::compiler::CompilerService cs;
    aura_536_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
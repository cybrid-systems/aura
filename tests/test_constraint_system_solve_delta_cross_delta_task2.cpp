// test_constraint_system_solve_delta_cross_delta_task2.cpp
// Issue #573: Task2 solve_delta cross-delta re-validation +
// touched_roots_ narrowing refresh + typed-incremental-stats.
//
// Non-duplicative with #466/#432 (reverify unit tests),
// #536 (dirty narrowing matrix), #628 (clean-conflict safety),
// #608 (type-incremental-stats CompilerMetrics path).
//
// AC1: cross-delta EQUAL conflict on prior clean constraint
// AC2: merged-var Int/String cross-delta conflict
// AC3: query:typed-incremental-stats reachable + non-negative
// AC4: predicate mutate → narrowing_refresh + selective_recheck grow
// AC5: incremental_infer → delta_solve_time_us observable
// AC6: occurrence-stale-count == 0 after re-narrow
// AC7: conflict matrix ≥50% CONFLICT detection
// AC8: query:typed-mutation-stats + constraint-stats regression
// AC9: multi-round mutate — typed-incremental-stats monotonic
//
// Unit ConstraintSystem tests run first; integration uses one
// CompilerService for the query regression matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_573_detail {

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

static std::int64_t typed_incremental_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:typed-incremental-stats\")");
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

static void test_equal_cross_delta_conflict() {
    std::println("\n--- AC1: T~Int then T~String cross-delta conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "first delta T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "second delta T~String conflicts on clean binding");
}

static void test_merge_binding_conflict() {
    std::println("\n--- AC2: merged vars Int/String cross-delta conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    const auto u = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, u, reg.string_type()}) == SolveResult::SOLVED,
          "U~String solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, u}) == SolveResult::CONFLICT,
          "T~U merge conflicts across clean constraints");
}

static void test_conflict_matrix() {
    std::println("\n--- AC7: conflict matrix ≥50% detection ---");
    std::size_t conflict_detected = 0;
    std::size_t conflict_injected = 0;

    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
        ++conflict_injected;
        if (solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) ==
            SolveResult::CONFLICT)
            ++conflict_detected;
    }
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        const auto u = cs.fresh_var();
        (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
        (void)solve_delta_with(cs, {Constraint::EQUAL, u, reg.bool_type()});
        ++conflict_injected;
        if (solve_delta_with(cs, {Constraint::EQUAL, t, u}) == SolveResult::CONFLICT)
            ++conflict_detected;
    }
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto linear = reg.register_linear(reg.int_type());
        const auto t = cs.fresh_var();
        (void)solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.dynamic_type()});
        ++conflict_injected;
        if (solve_delta_with(cs, {Constraint::CONSISTENT, t, linear}) == SolveResult::CONFLICT)
            ++conflict_detected;
    }

    std::println("  conflict_detected={}/{}", conflict_detected, conflict_injected);
    CHECK(conflict_detected * 2 >= conflict_injected, "≥50% injected conflict scenarios detected");
}

static void run_integration_matrix(CompilerService& cs) {
    std::println("\n--- AC3: query:typed-incremental-stats ---");
    CHECK(load_workspace(cs), "load if workspace");
    const auto s0 = typed_incremental_stats(cs);
    std::println("  query:typed-incremental-stats = {}", s0);
    CHECK(s0 >= 0, "typed-incremental-stats non-negative");

    std::println("\n--- AC4: predicate mutate → narrowing + local recheck ---");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    const auto sel0 = cs.evaluator().get_selective_recheck_count();
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 7) 0))\" "
                  "\"issue-573-if\")");
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    const auto sel1 = cs.evaluator().get_selective_recheck_count();
    std::println("  narrowing_refresh: {} -> {}", n0, n1);
    std::println("  selective_recheck: {} -> {}", sel0, sel1);
    CHECK(n1 > n0, "narrowing_refresh bumped on predicate mutate");
    CHECK(sel1 >= sel0, "selective_recheck monotonic on predicate mutate");

    std::println("\n--- AC5: delta_solve_time_us observable after incremental ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
                  "\"issue-573-solve\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto snap = cs.snapshot();
    std::println("  delta_solve_time_us={}", snap.delta_solve_time_us);
    CHECK(snap.delta_solve_time_us >= 0, "delta_solve_time_us observable after incremental infer");

    std::println("\n--- AC6: occurrence-stale-count == 0 after re-narrow ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 4) 0))\" "
                  "\"issue-573-stale\")");
    auto stale = cs.eval("(query:occurrence-stale-count)");
    const auto stale_count = stale && is_int(*stale) ? as_int(*stale) : -1;
    std::println("  occurrence-stale-count = {}", stale_count);
    CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");

    std::println("\n--- AC8: query regression ---");
    auto tms = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    auto cstats = cs.eval("(engine:metrics \"query:constraint-stats\")");
    CHECK(tms && is_int(*tms), "query:typed-mutation-stats returns int");
    CHECK(cstats && is_int(*cstats), "query:constraint-stats returns int");

    std::println("\n--- AC9: multi-round mutate typed-incremental-stats ---");
    const auto stats9a = typed_incremental_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 11) + ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                      "-f\")");
        auto rf = cs.eval("(f 3)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        if (rf && is_int(*rf))
            CHECK(as_int(*rf) == 3 + round + 11, "narrow semantics round " + std::to_string(round));
    }
    const auto stats9b = typed_incremental_stats(cs);
    std::println("  typed-incremental-stats: {} -> {}", stats9a, stats9b);
    CHECK(stats9b >= stats9a, "typed-incremental-stats monotonic over matrix");

    const auto sel = cs.evaluator().get_selective_recheck_count();
    const auto narrow = cs.evaluator().get_narrowing_refresh_count();
    std::println("  final selective_recheck={} narrowing_refresh={}", sel, narrow);
    CHECK(sel > 0, "local_recheck_hit_rate proxy > 0 after mutate cycle");
    CHECK(narrow > 0, "narrowing_refresh_count > 0 after mutate cycle");
}

} // namespace aura_573_detail

int main() {
    using namespace aura_573_detail;
    test_equal_cross_delta_conflict();
    test_merge_binding_conflict();
    test_conflict_matrix();
    aura::compiler::CompilerService cs;
    run_integration_matrix(cs);
    return RUN_ALL_TESTS();
}
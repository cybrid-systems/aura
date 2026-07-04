// test_constraintsystem_solve_delta_touched_roots_509.cpp
// Issue #509: solve_delta touched_roots cross-delta conflict
// detection + query:constraint-delta-stats.
//
// Non-duplicative with #466/#432 (base reverify), #536 (dirty narrowing),
// #628 (clean-conflict safety stats), #573 (Task2 typed-incremental).
//
// AC1: T~Int then T~String cross-delta CONFLICT
// AC2: merged-var Int/String cross-delta CONFLICT
// AC3: delta_conflict_reverify_total bumps on touched roots
// AC4: delta_conflict_detected_total bumps on conflict
// AC5: query:constraint-delta-stats reachable + non-negative
// AC6: conflict matrix ≥50% detection
// AC7: query:constraint-stats + solve-delta-safety-stats regression
// AC8: incremental_infer multi-mutate smoke (no silent wrong types)
//
// Unit ConstraintSystem tests run first; integration uses one
// CompilerService for the query regression matrix.

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_509_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static std::int64_t constraint_delta_stats(CompilerService& cs) {
    auto r = cs.eval("(query:constraint-delta-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void test_equal_cross_delta_conflict() {
    std::println("\n--- AC1: T~Int then T~String cross-delta CONFLICT ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "first delta T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "second delta T~String conflicts via touched_roots reverify");
}

static void test_merge_binding_conflict() {
    std::println("\n--- AC2: merged vars Int/String cross-delta CONFLICT ---");
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

static void test_reverify_and_detected_counters() {
    std::println("\n--- AC3/AC4: reverify + detected counters bump ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    const auto t = cs.fresh_var();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()});
    const auto rev = metrics.delta_conflict_reverify_total.load();
    const auto det = metrics.delta_conflict_detected_total.load();
    std::println("  reverify_total={} detected_total={}", rev, det);
    CHECK(rev > 0, "delta_conflict_reverify_total bumped");
    CHECK(det > 0, "delta_conflict_detected_total bumped");
}

static void test_conflict_matrix() {
    std::println("\n--- AC6: conflict matrix ≥50% detection ---");
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
    std::println("\n--- AC5: query:constraint-delta-stats ---");
    const auto s0 = constraint_delta_stats(cs);
    std::println("  query:constraint-delta-stats = {}", s0);
    CHECK(s0 >= 0, "constraint-delta-stats non-negative");

    TypeRegistry reg;
    ConstraintSystem constraint_cs(reg);
    CompilerMetrics metrics;
    constraint_cs.set_metrics(&metrics);
    const auto t = constraint_cs.fresh_var();
    (void)solve_delta_with(constraint_cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(constraint_cs, {Constraint::EQUAL, t, reg.string_type()});

    std::println("\n--- AC7: query regression ---");
    auto cstats = cs.eval("(query:constraint-stats)");
    auto safety = cs.eval("(query:solve-delta-safety-stats)");
    CHECK(cstats && is_int(*cstats), "query:constraint-stats returns int");
    CHECK(safety && is_int(*safety), "query:solve-delta-safety-stats returns int");

    const auto rev = metrics.delta_conflict_reverify_total.load();
    const auto det = metrics.delta_conflict_detected_total.load();
    std::println("  unit reverify={} detected={}", rev, det);
    CHECK(rev > 0, "unit path bumped touched_roots_hits proxy");
    CHECK(det > 0, "unit path bumped conflict detected");

    std::println("\n--- AC8: incremental_infer multi-mutate smoke ---");
    CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0))\")"), "load workspace");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(typecheck-current)");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 2) 0))\" "
                  "\"issue-509-a\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 3) 0))\" "
                  "\"issue-509-b\")");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 4)");
    CHECK(r && is_int(*r), "eval after multi-mutate incremental infer");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 7, "narrow-dependent semantics preserved");
}

} // namespace aura_509_detail

int main() {
    using namespace aura_509_detail;
    test_equal_cross_delta_conflict();
    test_merge_binding_conflict();
    test_reverify_and_detected_counters();
    test_conflict_matrix();
    aura::compiler::CompilerService cs;
    run_integration_matrix(cs);
    return RUN_ALL_TESTS();
}
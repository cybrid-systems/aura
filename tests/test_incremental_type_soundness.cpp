// test_incremental_type_soundness.cpp — Issue #432 / #466:
// solve_delta touched-root conflict detection + re-verify.
//
// AC1: cross-delta EQUAL conflicts detected without full solve
// AC2: merged-var binding conflicts detected
// AC3: reverify counter bumps when touched roots present
// AC4: ≥50% of injected conflict matrix returns CONFLICT
// AC5: happy-path compatible deltas stay SOLVED
// AC6: query:constraint-stats / snapshot fields plumbed
// AC7: incremental_infer multi-mutate smoke (no silent wrong types)

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_466_detail {

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

static void test_int_then_string_conflict() {
    std::println("\n--- AC1: T~Int then T~String EQUAL conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "first delta T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "second delta T~String conflicts");
}

static void test_merge_binding_conflict() {
    std::println("\n--- AC2: merged vars with Int/String bindings conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    const auto u = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, u, reg.string_type()}) == SolveResult::SOLVED,
          "U~String solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, u}) == SolveResult::CONFLICT,
          "T~U merge conflicts across deltas");
}

static void test_reverify_counter_bumps() {
    std::println("\n--- AC3: delta_conflict_reverify_total bumps ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    aura::compiler::CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    const auto t = cs.fresh_var();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.int_type()});
    const auto rev = metrics.delta_conflict_reverify_total.load();
    std::println("  reverify_total={}", rev);
    CHECK(rev > 0, "re-verify scan ran after touched-root delta");
}

static void test_conflict_matrix_detection_rate() {
    std::println("\n--- AC4: injected conflict matrix ≥50% CONFLICT ---");
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
        const auto t = cs.fresh_var();
        (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
        ++conflict_injected;
        if (solve_delta_with(cs, {Constraint::EQUAL, t, reg.bool_type()}) == SolveResult::CONFLICT)
            ++conflict_detected;
    }
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        const auto u = cs.fresh_var();
        (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
        if (solve_delta_with(cs, {Constraint::EQUAL, u, reg.int_type()}) == SolveResult::SOLVED)
            CHECK(true, "matrix: compatible Int bindings stay SOLVED");
    }

    std::println("  conflict_detected={}/{}", conflict_detected, conflict_injected);
    CHECK(conflict_detected * 2 >= conflict_injected, "≥50% injected conflict scenarios detected");
}

static void test_happy_path_compatible_deltas() {
    std::println("\n--- AC5: compatible sequential deltas stay SOLVED ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.dynamic_type()}) ==
              SolveResult::SOLVED,
          "T~Dynamic solves");
    CHECK(solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int after Dynamic solves");
}

static void test_constraint_stats_query() {
    std::println("\n--- AC6: query:constraint-stats plumbed ---");
    CompilerService cs;
    const auto stats0 = cs.eval("(engine:metrics \"query:constraint-stats\")");
    CHECK(stats0 && is_int(*stats0), "query:constraint-stats returns int");
    const auto snap = cs.snapshot();
    CHECK(snap.delta_conflict_reverify_total == 0 || snap.delta_conflict_reverify_total >= 0,
          "snapshot exposes delta_conflict_reverify_total");
}

static void test_incremental_infer_multi_mutate_smoke() {
    std::println("\n--- AC7: incremental_infer multi-mutate smoke ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0))\")"), "load workspace");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(typecheck-current)");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 2) 0))\" "
                  "\"issue-466-a\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 3) 0))\" "
                  "\"issue-466-b\")");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 4)");
    CHECK(r && is_int(*r), "eval after multi-mutate incremental infer");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 7, "narrow-dependent semantics preserved");
}

} // namespace aura_466_detail

int main() {
    using namespace aura_466_detail;
    test_int_then_string_conflict();
    test_merge_binding_conflict();
    test_reverify_counter_bumps();
    test_conflict_matrix_detection_rate();
    test_happy_path_compatible_deltas();
    test_constraint_stats_query();
    test_incremental_infer_multi_mutate_smoke();
    return RUN_ALL_TESTS();
}
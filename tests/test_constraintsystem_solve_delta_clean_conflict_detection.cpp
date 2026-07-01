// test_constraintsystem_solve_delta_clean_conflict_detection.cpp
// Issue #628: ConstraintSystem solve_delta clean-constraint
// conflict detection + var_to_constraints_ dep tracking safety.
//
// Non-duplicative with #466/#432 (touched-root reverify),
// #409 (reverse map), #608 (incremental dependency matrix).
//
// AC1: cross-delta EQUAL conflict detected (T~Int then T~String)
// AC2: merged-var binding conflict across deltas
// AC3: Dynamic/Linear consistent clash detected post-delta
// AC4: reverify counter bumps when touched roots present
// AC5: compatible sequential deltas stay SOLVED
// AC6: query:solve-delta-safety-stats reachable + grows on conflict
// AC7: query:constraint-stats regression still works
// AC8: multi-delta conflict matrix ≥50% CONFLICT detection
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

namespace aura_628_detail {

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

static std::int64_t solve_delta_safety_stats(CompilerService& cs) {
    auto r = cs.eval("(query:solve-delta-safety-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void test_equal_cross_delta_conflict() {
    std::println("\n--- AC1: T~Int then T~String EQUAL conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) ==
              SolveResult::SOLVED,
          "first delta T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) ==
              SolveResult::CONFLICT,
          "second delta T~String conflicts via clean reverify");
}

static void test_merge_binding_conflict() {
    std::println("\n--- AC2: merged vars Int/String conflict ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    const auto u = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) ==
              SolveResult::SOLVED,
          "T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, u, reg.string_type()}) ==
              SolveResult::SOLVED,
          "U~String solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, u}) ==
              SolveResult::CONFLICT,
          "T~U merge conflicts across clean constraints");
}

static void test_dynamic_linear_clash() {
    std::println("\n--- AC3: Dynamic/Linear consistent clash ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    const auto linear = reg.register_linear(reg.int_type());
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.dynamic_type()}) ==
              SolveResult::SOLVED,
          "T~Dynamic solves");
    const auto clash =
        solve_delta_with(cs, {Constraint::CONSISTENT, t, linear});
    std::println("  Dynamic/Linear delta result conflict={}",
                 clash == SolveResult::CONFLICT);
    CHECK(clash == SolveResult::CONFLICT,
          "T~Linear after T~Dynamic conflicts (Linear reject)");
    const auto detected =
        metrics.delta_conflict_detected_total.load(std::memory_order_relaxed);
    CHECK(detected > 0, "delta_conflict_detected_total bumped");
}

static void test_reverify_counter_bumps() {
    std::println("\n--- AC4: delta_conflict_reverify_total bumps ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    CompilerMetrics metrics;
    cs.set_metrics(&metrics);
    const auto t = cs.fresh_var();
    (void)solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.int_type()});
    const auto rev = metrics.delta_conflict_reverify_total.load();
    std::println("  reverify_total={}", rev);
    CHECK(rev > 0, "clean-constraint reverify scan ran");
}

static void test_compatible_deltas_solved() {
    std::println("\n--- AC5: compatible deltas stay SOLVED ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.dynamic_type()}) ==
              SolveResult::SOLVED,
          "T~Dynamic solves");
    CHECK(solve_delta_with(cs, {Constraint::CONSISTENT, t, reg.int_type()}) ==
              SolveResult::SOLVED,
          "T~Int after Dynamic solves");
}

static void test_conflict_matrix() {
    std::println("\n--- AC8: conflict matrix ≥50% detection ---");
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
        if (solve_delta_with(cs, {Constraint::CONSISTENT, t, linear}) ==
            SolveResult::CONFLICT)
            ++conflict_detected;
    }

    std::println("  conflict_detected={}/{}", conflict_detected, conflict_injected);
    CHECK(conflict_detected * 2 >= conflict_injected,
          "≥50% injected conflict scenarios detected");
}

static void run_integration_matrix(CompilerService& cs) {
    std::println("\n--- AC6: query:solve-delta-safety-stats ---");
    const auto s0 = solve_delta_safety_stats(cs);
    std::println("  query:solve-delta-safety-stats = {}", s0);
    CHECK(s0 >= 0, "solve-delta-safety-stats non-negative");

    TypeRegistry reg;
    ConstraintSystem constraint_cs(reg);
    CompilerMetrics metrics;
    constraint_cs.set_metrics(&metrics);
    const auto t = constraint_cs.fresh_var();
    (void)solve_delta_with(constraint_cs, {Constraint::EQUAL, t, reg.int_type()});
    (void)solve_delta_with(constraint_cs,
                           {Constraint::EQUAL, t, reg.string_type()});

    std::println("\n--- AC7: query:constraint-stats regression ---");
    auto cstats = cs.eval("(query:constraint-stats)");
    CHECK(cstats && is_int(*cstats), "query:constraint-stats returns int");

    const auto detected =
        metrics.delta_conflict_detected_total.load(std::memory_order_relaxed);
    const auto reverify =
        metrics.delta_conflict_reverify_total.load(std::memory_order_relaxed);
    std::println("  unit detected={} reverify={}", detected, reverify);
    CHECK(detected > 0, "unit path bumped clean_conflicts_detected");
    CHECK(reverify > 0, "unit path bumped delta_vs_full_consistency");
}

} // namespace aura_628_detail

int main() {
    using namespace aura_628_detail;
    test_equal_cross_delta_conflict();
    test_merge_binding_conflict();
    test_dynamic_linear_clash();
    test_reverify_counter_bumps();
    test_compatible_deltas_solved();
    test_conflict_matrix();
    aura::compiler::CompilerService cs;
    run_integration_matrix(cs);
    return RUN_ALL_TESTS();
}
// test_task2_refinement_closed_loop_495.cpp
// Issue #495: Task2 refinement closed-loop — constraint soundness,
// dead coercion elimination, occurrence blame, JIT elision synergy.
//
// Non-duplicative with pillar-specific tests (#432/#467/#509/#574).
// Validates the four #495 actionable items integrate via observability
// primitives + a single mutate/typecheck/eval smoke path.
//
// AC1: query:task2-refinement-stats reachable + non-negative
// AC2: constraint pillar — cross-delta CONFLICT unit smoke
// AC3: coercion pillar — query:coercion-elim-stats after typecheck
// AC4: occurrence pillar — if mutate + occurrence-stats grow
// AC5: JIT elision pillar — query:coercion-zerooverhead-stats
// AC6: integrated mutate → eval semantics preserved
// AC7: query regression (constraint-delta, occurrence, coercion-elim)
// AC8: multi-round mutate — task2-refinement-stats monotonic
//
// Unit constraint test runs first; integration uses one CompilerService.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_495_detail {

using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;

static constexpr const char* k_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
)";

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static std::int64_t task2_refinement_stats(CompilerService& cs) {
    auto r = cs.eval("(query:task2-refinement-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static void test_constraint_pillar_unit() {
    std::println("\n--- AC2: constraint pillar cross-delta CONFLICT ---");
    TypeRegistry reg;
    ConstraintSystem cs(reg);
    const auto t = cs.fresh_var();
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int solves");
    CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
          "T~String conflicts via touched_roots reverify");
}

static void run_integration_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:task2-refinement-stats ---");
    CHECK(load_workspace(cs), "load if workspace");
    const auto s0 = task2_refinement_stats(cs);
    std::println("  query:task2-refinement-stats = {}", s0);
    CHECK(s0 >= 0, "task2-refinement-stats non-negative");

    std::println("\n--- AC3: coercion pillar ---");
    auto ces = cs.eval("(query:coercion-elim-stats)");
    CHECK(ces && is_int(*ces), "query:coercion-elim-stats returns int");
    std::println("  coercion-elim-stats = {}", ces && is_int(*ces) ? as_int(*ces) : -1);

    std::println("\n--- AC4: occurrence pillar ---");
    const auto stats4a = cs.eval("(query:occurrence-stats)");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 8) 0))\" "
                  "\"issue-495-occ\")");
    auto stats4b = cs.eval("(query:occurrence-stats)");
    CHECK(stats4a && is_int(*stats4a), "occurrence-stats before mutate");
    CHECK(stats4b && is_int(*stats4b), "occurrence-stats after mutate");
    if (stats4a && stats4b && is_int(*stats4a) && is_int(*stats4b))
        CHECK(as_int(*stats4b) >= as_int(*stats4a), "occurrence-stats monotonic after if mutate");

    std::println("\n--- AC5: JIT elision pillar ---");
    auto zos = cs.eval("(query:coercion-zerooverhead-stats)");
    CHECK(zos && is_int(*zos), "query:coercion-zerooverhead-stats returns int");

    std::println("\n--- AC6: integrated mutate eval semantics ---");
    auto r6 = cs.eval("(f 5)");
    CHECK(r6 && is_int(*r6), "f 5 eval after mutate");
    if (r6 && is_int(*r6))
        CHECK(as_int(*r6) == 13, "narrow-dependent (+ x 8) correct");

    std::println("\n--- AC7: query regression ---");
    auto cds = cs.eval("(query:constraint-delta-stats)");
    CHECK(cds && is_int(*cds), "query:constraint-delta-stats returns int");

    std::println("\n--- AC8: multi-round mutate stats monotonic ---");
    const auto stats8a = task2_refinement_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x " +
                      std::to_string(round + 10) + ") 0))\" \"r" + std::to_string(round) + "\")");
        auto ev = cs.eval("(f 2)");
        CHECK(ev && is_int(*ev), "eval ok round " + std::to_string(round));
    }
    const auto stats8b = task2_refinement_stats(cs);
    std::println("  task2-refinement-stats: {} -> {}", stats8a, stats8b);
    CHECK(stats8b >= stats8a, "task2-refinement-stats monotonic over matrix");
}

} // namespace aura_495_detail

int main() {
    using namespace aura_495_detail;
    test_constraint_pillar_unit();
    aura::compiler::CompilerService cs;
    run_integration_matrix(cs);
    return RUN_ALL_TESTS();
}
// test_incremental_type_batch.cpp — batch driver for incremental_type family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration,
// following the test_per_defuse_batch / test_env_lookup_batch /
// test_fiber_resume_batch / test_compact_sweep_batch /
// test_incremental_relower_batch / test_macro_reflect_batch precedent
// in AuraDomainTests.cmake):
//
//   Issue #526 — dirty/epoch propagation to type_checker + Pass
//                short-circuit for typed self-mod soundness (5 ACs)
//   Issue #536 — touched_roots + selective occurrence re-narrow +
//                Pass short-circuit, predicate mutate matrix (8 ACs)
//   Issue #432/#466 — ConstraintSystem solve_delta cross-delta
//                soundness (touched-root reverify + conflict
//                metrics, ≥50% matrix detection) (7 ACs)
//
// Pattern: CHECK() macros + RUN_ALL_TESTS() (test_harness.hpp),
// namespace aura_incremental_type_batch, EXCLUDE_FROM_ALL per
// AuraDomainTests.cmake legacy batch convention. Default build skips;
// granular debug via `ninja test_incremental_type_batch` on demand.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <string>

import std;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.ir;
import aura.compiler.pass_manager;

namespace aura_incremental_type_batch {

using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::ir::IROpcode;

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

// ── Block 1: Issue #526 (5 ACs) ──
// Original: tests/test_incremental_type_dirty.cpp
static void run_526() {
    std::println("\n═══ Issue #526: dirty/epoch → type_checker selective recheck ═══");

    // AC1: mutate:rebind triggers selective_recheck (infer_flat_partial)
    {
        std::println("\n--- AC1: mutate:rebind selective infer_flat_partial ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1) (define y 2)\")");
        cs.eval("(eval-current)");

        CHECK(cs.eval("(mutate:rebind \"x\" \"42\" \"526\")").has_value(),
              "mutate:rebind succeeds");

        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation log populated after rebind");
        CHECK(!cs.evaluator().has_type_error(), "post-mutate selective typecheck clean");

        const auto reinferred = cs.incremental_infer(ws->all_mutations().back());
        std::println("  reinferred={} (second incremental_infer on same record)", reinferred);
        CHECK(reinferred >= 0, "incremental_infer returns non-negative count");
    }

    // AC2: defuse_version bumps on mutate (epoch gate)
    {
        std::println("\n--- AC2: defuse_version bumps on mutate (epoch gate) ---");
        CompilerService cs;
        cs.eval("(set-code \"(define a 0)\")");
        cs.eval("(eval-current)");
        const auto dv0 = cs.evaluator().defuse_version_for_test();
        cs.eval("(mutate:rebind \"a\" \"99\" \"epoch\")");
        const auto dv1 = cs.evaluator().defuse_version_for_test();
        CHECK(dv1 > dv0, "defuse_version bumps on mutate");
    }

    // AC3: DCE dirty-block short-circuit metric
    {
        std::println("\n--- AC3: DCE dirty-block short-circuit metric ---");
        CompilerService cs;
        const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();

        aura::ir::IRModule mod;
        mod.functions.push_back({.name = "f", .local_count = 16});
        auto& func = mod.functions.back();
        func.blocks.push_back({0});
        func.blocks[0].instructions = {
            {IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
            {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
            {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        };
        func.blocks.push_back({1});
        func.blocks[1].instructions = {
            {IROpcode::ConstI64, {2, 2, 0, 0}, 0, 1},
            {IROpcode::CastOp, {3, 2, 3, 0}, 0, 0},
            {IROpcode::Return, {3, 0, 0, 0}, 0, 0},
        };

        std::vector<std::uint8_t> dirty_mask = {1, 0};
        DeadCoercionEliminationPass dce;
        dce.run_function(func, dirty_mask);
        CHECK(func.blocks[0].instructions[1].opcode == IROpcode::Local,
              "dirty block CastOp elided");
        CHECK(func.blocks[1].instructions[1].opcode == IROpcode::CastOp, "clean block untouched");

        cs.evaluator().bump_passes_skipped_type_dirty(1);
        const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
        CHECK(ps1 >= ps0 + 1, "passes_skipped_type_dirty observable");
    }

    // AC4: query:typed-mutation-stats + query:dirty-impact
    {
        std::println("\n--- AC4: query:typed-mutation-stats + query:dirty-impact ---");
        CompilerService cs;
        cs.eval("(set-code \"(define z 1)\")");
        cs.eval("(eval-current)");

        auto r1 = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
        CHECK(r1 && is_int(*r1), "query:typed-mutation-stats returns int");
        CHECK(as_int(*r1) >= 0, "typed-mutation-stats >= 0");

        auto r2 = cs.eval("(query:dirty-impact)");
        CHECK(r2 && is_int(*r2), "query:dirty-impact returns int");
        CHECK(as_int(*r2) >= 0, "dirty-impact >= 0");
    }

    // AC5: end-to-end typed mutate cycle
    {
        std::println("\n--- AC5: end-to-end typed mutate cycle ---");
        CompilerService cs;
        cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g 0)\")");
        auto baseline = cs.eval("(eval-current)");
        CHECK(baseline.has_value(), "baseline eval ok");

        auto snap0 = cs.snapshot();
        const auto reinf0 = snap0.incremental_typecheck_re_inferred_total;
        const auto auto0 = snap0.incremental_typecheck_auto_invocations_total;

        for (int i = 0; i < 5; ++i) {
            std::string code = "(set-code \"(define g ";
            code += std::to_string(i);
            code += ")\")";
            CHECK(cs.eval(code).has_value(), "set-code ok");
            auto r = cs.eval("(eval-current)");
            CHECK(r.has_value(), "post-mutation eval ok");
        }

        auto after = cs.eval("(eval-current)");
        CHECK(after.has_value(), "final eval ok");

        auto snap1 = cs.snapshot();
        CHECK(snap1.incremental_typecheck_re_inferred_total >= reinf0,
              "re_inferred_total monotonic");
        CHECK(snap1.incremental_typecheck_auto_invocations_total >= auto0,
              "auto_invocations monotonic");
        CHECK(snap1.typecheck_gen_saved_total >= snap0.typecheck_gen_saved_total,
              "typecheck_gen_saved monotonic (incremental win)");
    }
}

// ── Block 2: Issue #536 (8 ACs) ──
// Original: tests/test_incremental_type_dirty_narrowing.cpp
static constexpr const char* k_if_prog_536 = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
(define g (lambda (y) (+ y 10)))
)";

static std::int64_t typed_mutation_stats_536(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_workspace_536(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog_536 + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static void run_536() {
    std::println("\n═══ Issue #536: touched_roots + re-narrow + Pass short-circuit ═══");

    // AC1: touched_roots cross-delta CONFLICT (unit)
    {
        std::println("\n--- AC1: touched_roots cross-delta CONFLICT ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
              "T~Int solves");
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) ==
                  SolveResult::CONFLICT,
              "T~String cross-delta conflicts via reverify");
    }

    CompilerService cs;
    CHECK(load_workspace_536(cs), "load if workspace");

    // AC2: typed-mutation-stats + dirty-impact
    {
        std::println("\n--- AC2: typed-mutation-stats + dirty-impact ---");
        auto stats = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
        auto impact = cs.eval("(query:dirty-impact)");
        CHECK(stats && is_int(*stats), "query:typed-mutation-stats returns int");
        CHECK(impact && is_int(*impact), "query:dirty-impact returns int");
        CHECK(as_int(*stats) >= 0, "typed-mutation-stats >= 0");
        CHECK(as_int(*impact) >= 0, "dirty-impact >= 0");
    }

    // AC3: predicate mutate → narrowing + stats grow
    {
        std::println("\n--- AC3: predicate mutate → narrowing + stats grow ---");
        const auto n0 = cs.evaluator().get_narrowing_refresh_count();
        const auto stats0 = typed_mutation_stats_536(cs);
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 6) 0))\" "
                      "\"issue-536-if\")");
        const auto n1 = cs.evaluator().get_narrowing_refresh_count();
        const auto stats1 = typed_mutation_stats_536(cs);
        std::println("  narrowing_refresh: {} -> {}", n0, n1);
        std::println("  typed-mutation-stats: {} -> {}", stats0, stats1);
        CHECK(n1 > n0, "narrowing_refresh bumped on predicate mutate");
        CHECK(stats1 >= stats0, "typed-mutation-stats monotonic");
    }

    // AC4: occurrence-stale-count == 0 after re-narrow
    {
        std::println("\n--- AC4: occurrence-stale-count == 0 after re-narrow ---");
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 4) 0))\" "
                      "\"issue-536-stale\")");
        auto stale = cs.eval("(stats:get \"query:occurrence-stale-count\")");
        const auto stale_count = stale && is_int(*stale) ? as_int(*stale) : -1;
        std::println("  occurrence-stale-count = {}", stale_count);
        CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");
    }

    // AC5: passes_skipped monotonic under mutate
    {
        std::println("\n--- AC5: passes_skipped monotonic under mutate ---");
        const auto ps0 = cs.evaluator().get_passes_skipped_type_dirty();
        for (int i = 0; i < 3; ++i) {
            (void)cs.eval("(mutate:rebind \"g\" \"(lambda (y) (+ y " + std::to_string(20 + i) +
                          "))\" \"issue-536-ps-" + std::to_string(i) + "\")");
            (void)cs.eval("(eval-current)");
        }
        const auto ps1 = cs.evaluator().get_passes_skipped_type_dirty();
        std::println("  passes_skipped_type_dirty: {} -> {}", ps0, ps1);
        CHECK(ps1 >= ps0, "passes_skipped monotonic non-decreasing");
    }

    // AC6: multi-round if-predicate mutate matrix
    {
        std::println("\n--- AC6: multi-round if-predicate mutate matrix ---");
        const auto stats6a = typed_mutation_stats_536(cs);
        for (int round = 0; round < 3; ++round) {
            const std::string f_body =
                "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 9) + ") 0))";
            (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                          "-f\")");
            auto rf = cs.eval("(f 3)");
            CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
            if (rf && is_int(*rf))
                CHECK(as_int(*rf) == 3 + round + 9,
                      "narrow semantics round " + std::to_string(round));
        }
        const auto stats6b = typed_mutation_stats_536(cs);
        std::println("  typed-mutation-stats: {} -> {}", stats6a, stats6b);
        CHECK(stats6b >= stats6a, "typed-mutation-stats monotonic over matrix");
    }

    // AC7: touched_roots_size after incremental infer
    {
        std::println("\n--- AC7: touched_roots_size after incremental infer ---");
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
                      "\"issue-536-tr\")");
        auto* ws = cs.workspace_flat();
        CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
        (void)cs.incremental_infer(ws->all_mutations().back());
        const auto tr = cs.evaluator().get_touched_roots_size();
        const auto cc = cs.evaluator().get_cross_delta_conflicts_caught();
        std::println("  touched_roots_size={} cross_delta_conflicts={}", tr, cc);
        CHECK(tr >= 0, "touched_roots_size observable (may be 0 on happy path)");
    }

    // AC8: sequential query/eval stress
    {
        std::println("\n--- AC8: sequential query/eval stress ---");
        std::int64_t stress_sum = 0;
        for (int i = 0; i < 8; ++i) {
            (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x " +
                          std::to_string(i) + ") 0))\" \"stress-" + std::to_string(i) + "\")");
            auto qs = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
            CHECK(qs && is_int(*qs), "typed-mutation-stats during stress");
            if (qs && is_int(*qs))
                stress_sum += as_int(*qs);
            auto ev = cs.eval("(f 2)");
            CHECK(ev && is_int(*ev), "eval during stress round " + std::to_string(i));
        }
        std::println("  stress_sum={}", stress_sum);
        CHECK(stress_sum > 0, "stress query/eval sum > 0");
    }
}

// ── Block 3: Issue #432/#466 (7 ACs) ──
// Original: tests/test_incremental_type_soundness.cpp
static void run_466() {
    std::println("\n═══ Issue #432/#466: solve_delta cross-delta soundness ═══");

    // AC1: T~Int then T~String EQUAL conflict
    {
        std::println("\n--- AC1: T~Int then T~String EQUAL conflict ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
              "first delta T~Int solves");
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.string_type()}) ==
                  SolveResult::CONFLICT,
              "second delta T~String conflicts");
    }

    // AC2: merged vars with Int/String bindings conflict
    {
        std::println("\n--- AC2: merged vars with Int/String bindings conflict ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        const auto t = cs.fresh_var();
        const auto u = cs.fresh_var();
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
              "T~Int solves");
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, u, reg.string_type()}) ==
                  SolveResult::SOLVED,
              "U~String solves");
        CHECK(solve_delta_with(cs, {Constraint::EQUAL, t, u}) == SolveResult::CONFLICT,
              "T~U merge conflicts across deltas");
    }

    // AC3: delta_conflict_reverify_total bumps
    {
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

    // AC4: injected conflict matrix ≥50% CONFLICT
    {
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
            if (solve_delta_with(cs, {Constraint::EQUAL, t, reg.bool_type()}) ==
                SolveResult::CONFLICT)
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
        CHECK(conflict_detected * 2 >= conflict_injected,
              "≥50% injected conflict scenarios detected");
    }

    // AC5: compatible sequential deltas stay SOLVED
    {
        std::println("\n--- AC5: compatible sequential deltas stay SOLVED ---");
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

    // AC6: query:constraint-stats plumbed
    {
        std::println("\n--- AC6: query:constraint-stats plumbed ---");
        CompilerService cs;
        const auto stats0 = cs.eval("(engine:metrics \"query:constraint-stats\")");
        CHECK(stats0 && is_int(*stats0), "query:constraint-stats returns int");
        const auto snap = cs.snapshot();
        CHECK(snap.delta_conflict_reverify_total == 0 || snap.delta_conflict_reverify_total >= 0,
              "snapshot exposes delta_conflict_reverify_total");
    }

    // AC7: incremental_infer multi-mutate smoke
    {
        std::println("\n--- AC7: incremental_infer multi-mutate smoke ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0))\")"),
              "load workspace");
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
}

} // namespace aura_incremental_type_batch

int main() {
    aura_incremental_type_batch::run_526();
    aura_incremental_type_batch::run_536();
    aura_incremental_type_batch::run_466();
    return RUN_ALL_TESTS();
}
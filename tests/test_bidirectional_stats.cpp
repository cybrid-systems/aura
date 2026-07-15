// tests/test_bidirectional_stats.cpp — Issue #1420 AC3:
// (compile:bidirectional-stats) EDSL primitive. Surfaces
// InferenceEngine::check_flat_call annotation contract counters
// (compile_bidirectional_check_call_total +
//  compile_bidirectional_annotation_pass_total +
//  compile_bidirectional_annotation_fail_total +
//  compile_bidirectional_coercion_deferred_total) and the
// persistent CompilerService bidirectional_mode_ flag, plus the
// pre-existing check_mode_narrow_hits_total reused for the
// :narrow-records slot.
//
// Production code (no new bidirectional check plumbing — that
// was already shipped in #384 first slice + #1413 4-AC contract
// test via check_flat_call + cs_.consistent_unify + is_coercible):
//   - src/compiler/observability_metrics.h: 4 new atomic fields
//     under the `compile_bidirectional_*` prefix (lands them in
//     the `compile` group of the (engine:metrics) facade via
//     metrics_group_for_field).
//   - src/compiler/type_checker_impl.cpp:4590: bump counters at
//     check_flat_call entry + pass / coerce / fail branches.
//   - src/compiler/evaluator.ixx: get_bidirectional_stats_fn_
//     field + setter (packed uint64 layout).
//   - src/compiler/service.ixx: hook captures `this`, reads
//     metrics_; CompilerService::bidirectional_mode() accessor
//     exposes the persistent flag.
//   - src/compiler/evaluator_primitives_compile_07.cpp (p63):
//     (compile:bidirectional-stats) primitive — default tier
//     (kPrimSecSafe), read-only.
//
// ACs:
//   AC1: (compile:bidirectional-stats) on a fresh service returns
//        :mode = "full" and all 4 counters = 0 (no eval yet).
//   AC2: After evaluating a matched annotation
//        (let ((x : Integer 1)) (+ x 2)), check-calls >= 1
//        and annotation-passes >= 1.
//   AC3: After evaluating a mismatched annotation
//        (let ((x : Integer "hello")) (+ x 2)), annotation-fails
//        >= 1.
//   AC4: (engine:metrics :group "compile") exposes
//        compile_bidirectional_check_call_total (the new field
//        landed in the :compile group of the facade via prefix
//        rule).
//
// Sampled mode is a follow-up — requires bidirectional_mode_
// bool→enum upgrade in type_checker.ixx, deferred to keep this
// ship scope-limited.

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.service;
import aura.diag;

namespace test_bidirectional_stats_detail {

// Helper: run a piece of code through the full CompilerService
// pipeline. Returns true if eval succeeded (no TypeError
// diagnostic), false otherwise. Mirrors test_bidirectional_annotation's
// run_eval so behavior is identical for the shared annotation
// paths (the bidirectional check plumbing is what #1413 already
// covered — this test focuses on the observability counters).
bool run_eval(aura::compiler::CompilerService& cs, const std::string& code) {
    auto set_r = cs.eval("(set-code \"" + code + "\")");
    if (!set_r.has_value())
        return false;
    auto eval_r = cs.eval("(eval-current)");
    return eval_r.has_value();
}

// Helper: read an int-valued key from a hash-returning expr.
// Mirrors test_engine_metrics_facade.cpp's hash_int helper.
std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view expr,
                      std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Helper: compare a string-valued hash key to an expected
// value. Avoids needing direct access to the Evaluator's
// string_heap_ from C++ — eval the comparison inside Aura.
bool hash_str_eq(aura::compiler::CompilerService& cs, std::string_view expr, std::string_view key,
                 std::string_view expected) {
    auto r = cs.eval(std::format("(string=? (hash-ref {} \"{}\") \"{}\")", expr, key, expected));
    if (!r || !aura::compiler::types::is_bool(*r))
        return false;
    return aura::compiler::types::as_bool(*r);
}

} // namespace test_bidirectional_stats_detail

int aura_issue_1420_run() {
    using namespace test_bidirectional_stats_detail;
    std::println("=== Issue #1420 AC3: (compile:bidirectional-stats) primitive ===");

    // ── AC1: default state on fresh CompilerService ──
    //
    // No eval yet → all 4 counters should be 0; mode is the
    // persistent CompilerService flag default (true → "full").
    {
        std::println("\n--- AC1: default state ---");
        aura::compiler::CompilerService cs;

        CHECK(hash_str_eq(cs, "(compile:bidirectional-stats)", "mode", "full"),
              "AC1a: :mode defaults to 'full' (persistent CompilerService flag)");
        CHECK(hash_int(cs, "(compile:bidirectional-stats)", "check-calls") == 0,
              "AC1b: :check-calls starts at 0");
        CHECK(hash_int(cs, "(compile:bidirectional-stats)", "annotation-passes") == 0,
              "AC1c: :annotation-passes starts at 0");
        CHECK(hash_int(cs, "(compile:bidirectional-stats)", "annotation-fails") == 0,
              "AC1d: :annotation-fails starts at 0");
        CHECK(hash_int(cs, "(compile:bidirectional-stats)", "coercion-deferred") == 0,
              "AC1e: :coercion-deferred starts at 0");
    }

    // ── AC2: matched annotation increments check-calls + passes ──
    //
    // (let ((x : Integer 1)) (+ x 2)) — synth of 1 is Int,
    // ⊆ annotation : Integer, body synth Int. bidirectional
    // check fires through check_flat_call (call-site expected
    // type is the let annotation type via the body). Both
    // counters must increment.
    {
        std::println("\n--- AC2: matched annotation ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer 1)) (+ x 2))");
        CHECK(ok, "AC2a: matched annotation typechecks OK");

        const auto check_calls = hash_int(cs, "(compile:bidirectional-stats)", "check-calls");
        const auto passes = hash_int(cs, "(compile:bidirectional-stats)", "annotation-passes");
        CHECK(check_calls >= 1,
              std::format("AC2b: :check-calls >= 1 after matched eval (got {})", check_calls));
        CHECK(passes >= 1,
              std::format("AC2c: :annotation-passes >= 1 after matched eval (got {})", passes));
    }

    // ── AC3: mismatched annotation increments annotation-fails ──
    //
    // (let ((x : Integer "hello")) (+ x 2)) — synth of "hello"
    // is String, ⊄ annotation : Integer. consistent_unify fails,
    // is_coercible is false (Int↔String is not gradual-coercible
    // by default), TypeError reported. annotation-fails must
    // increment.
    {
        std::println("\n--- AC3: mismatched annotation ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer \"hello\")) (+ x 2))");
        CHECK(!ok, "AC3a: mismatched annotation rejected (TypeError)");

        const auto fails = hash_int(cs, "(compile:bidirectional-stats)", "annotation-fails");
        const auto check_calls = hash_int(cs, "(compile:bidirectional-stats)", "check-calls");
        CHECK(fails >= 1,
              std::format("AC3b: :annotation-fails >= 1 after mismatched eval (got {})", fails));
        CHECK(check_calls >= 1,
              std::format("AC3c: :check-calls >= 1 (entry counter, regardless of outcome) (got {})",
                          check_calls));
    }

    // ── AC4: (engine:metrics :group "compile") exposes the new fields ──
    //
    // The compile_bidirectional_* prefix matches the `compile_`
    // rule in metrics_group_for_field (evaluator_primitives_obs_jit_01.cpp
    // line ~432), so the new atomics land in the `compile`
    // group of the engine:metrics facade (#1433 schema 2).
    // spot-check one of the four fields plus verify the group
    // is hash-typed.
    {
        std::println("\n--- AC4: engine:metrics facade exposes the new fields ---");
        aura::compiler::CompilerService cs;
        // Drive at least one bidirectional call so the counter
        // is non-zero on the facade read (proves both the wiring
        // and the prefix→group mapping).
        (void)run_eval(cs, "(let ((x : Integer 1)) (+ x 2))");

        auto group_r = cs.eval("(engine:metrics :group \"compile\")");
        CHECK(group_r && aura::compiler::types::is_hash(*group_r),
              "AC4a: (engine:metrics :group \"compile\") returns a hash");

        auto field_r = cs.eval("(hash-ref (engine:metrics :group \"compile\") "
                               "\"compile_bidirectional_check_call_total\")");
        CHECK(field_r && aura::compiler::types::is_int(*field_r),
              "AC4b: compile_bidirectional_check_call_total present in :compile group");
        if (field_r && aura::compiler::types::is_int(*field_r)) {
            const auto v = aura::compiler::types::as_int(*field_r);
            CHECK(v >= 1,
                  std::format(
                      "AC4c: compile_bidirectional_check_call_total >= 1 after eval (got {})", v));
        }
    }

    if (g_failed == 0) {
        std::println("\n=== ALL 4 ACs PASS ===");
        return 0;
    }
    std::println("\n=== {} ACs FAILED ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1420_run();
}
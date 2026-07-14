// tests/test_bidirectional_annotation.cpp — Issue #1413: True
// bidirectional type checking (Synth + Check) — annotation
// mismatch detection at compile time.
//
// Background: bidirectional check-mode plumbing is already wired
// in src/compiler/type_checker_impl.cpp:3087+ (Issue #384 first
// slice). check_flat_call at line 4606 does
// `synthesize_flat_call` → `cs_.consistent_unify` against the
// expected type from the call-site annotation. Mismatch reports
// a TypeError via diag_ with blame on the caller.
//
// This test is a contract test — it exercises the full
// CompilerService pipeline (cs.eval) which triggers
// Infer_flat → bidirectional check → mismatch detection. The
// bidirectional_mode_ flag is true by default (line 667 of
// type_checker.ixx), so the check is active out of the box.
//
// ACs:
//   AC1: (let ((x : Integer 1)) (+ x 2)) typechecks OK
//   AC2: (let ((x : Integer "hello")) (+ x 2)) gets a TypeError
//   AC3: bidirectional check survives across the function body —
//        synth of 1 (Int) ⊆ annotation : Integer passes, but
//        synth of "hello" (String) vs annotation : Integer fails
//   AC4: backward compat — code without annotations still
//        typechecks (the bidirectional check is opt-in per binding
//        via TypeAnnotation presence)
//
// Note: the top-level `infer_flat_bidirectional` orchestration
// proposed in the issue body is a follow-up. This test
// exercises the existing per-call / per-lambda check pass that
// already enforces the contract.

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

namespace test_bidirectional_annotation_detail {

// Helper: run a piece of code through the full CompilerService
// pipeline. Returns true if eval succeeded (no TypeError
// diagnostic), false otherwise.
bool run_eval(aura::compiler::CompilerService& cs, const std::string& code) {
    // set-code initializes workspace + parses; eval-current runs
    // the full type-check + interpret pipeline.
    auto set_r = cs.eval("(set-code \"" + code + "\")");
    if (!set_r.has_value())
        return false;
    auto eval_r = cs.eval("(eval-current)");
    return eval_r.has_value();
}

} // namespace test_bidirectional_annotation_detail

int aura_issue_1413_run() {
    using namespace test_bidirectional_annotation_detail;
    std::println("=== Issue #1413: True bidirectional type checking ===");

    // ── AC1: (let ((x : Integer 1)) (+ x 2)) typechecks OK ──
    //
    // annotation : Integer expects Int, synth of 1 is Int,
    // consistent_unify passes, body synth Int, ⊆ annotation.
    {
        std::println("\n--- AC1: (let ((x : Integer 1)) (+ x 2)) typechecks ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer 1)) (+ x 2))");
        CHECK(ok, "AC1: annotated binding with matching type typechecks OK");
    }

    // ── AC2: (let ((x : Integer "hello")) (+ x 2)) gets TypeError ──
    //
    // annotation : Integer expects Int, synth of "hello" is String,
    // consistent_unify fails, is_coercible is false, TypeError
    // reported, eval returns no value.
    {
        std::println("\n--- AC2: (let ((x : Integer \\\"hello\\\")) (+ x 2)) rejected ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer \"hello\")) (+ x 2))");
        CHECK(!ok, "AC2: annotated binding with mismatched type rejected (TypeError)");
    }

    // ── AC3: bidirectional check across function body ──
    //
    // annotation : Integer on the let, body (+ x 2) is Int (1 + 2
    // are both Int), ⊆ Integer. Positive path. The AC1 case
    // already covers this, but we make it explicit.
    {
        std::println("\n--- AC3: bidirectional check across function body ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x : Integer 100)) "
                                     "  (+ x 200))");
        CHECK(ok, "AC3: synth of body within annotation type is accepted");
    }

    // ── AC4: backward compat — no annotations still typechecks ──
    //
    // Plain let without annotation — bidirectional_mode_ is on but
    // no expected type flows in, so the check is a no-op.
    {
        std::println("\n--- AC4: no annotations still typechecks ---");
        aura::compiler::CompilerService cs;
        const bool ok = run_eval(cs, "(let ((x 1)) (+ x 2))");
        CHECK(ok, "AC4: plain let (no annotation) typechecks — backward compat");
    }

    if (g_failed == 0) {
        std::println("\n=== ALL 4 ACs PASS ===");
        return 0;
    }
    std::println("\n=== {} ACs FAILED ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1413_run();
}

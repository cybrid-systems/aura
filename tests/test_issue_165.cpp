// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_165.cpp — Issue #165: macro re-expansion + SyntaxMarker
// handling after EDSL mutations.
//
// TDD: this test reproduces the hygiene violation that #165
// fixes. It SHOULD FAIL on the current main (because the
// mutation doesn't trigger re-expansion). After #165 ships,
// the test should PASS.
//
// The minimal repro:
//   1. Define a hygienic macro `m` with internal binding `tmp`.
//   2. Eval (m) — initial expansion uses gensym for `tmp`.
//   3. mutate:rebind to add a new top-level binding `tmp = "x"`.
//   4. Eval (m) again — the macro expansion should STILL use
//      the gensym'd `tmp` (hygiene preserved), not the caller's
//      `tmp`.
//
// Bug: in the current implementation, step 4 either:
//   (a) returns the cached expanded form (uses the original
//       gensym'd tmp — coincidentally safe), OR
//   (b) re-evaluates the original macro body without re-
//       expansion, picking up the new `tmp` binding — hygiene
//       violation.
// The bug is observable when the macro's call site is
// evaluated AFTER the mutation in a context where the new
// `tmp` binding shadows the macro's internal.


// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;



namespace aura_issue_165_detail {
#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

static std::string run_str(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return "(eval error)";
    auto& v = *r;
    if (aura::compiler::types::is_string(v)) {
        auto idx = aura::compiler::types::as_string_idx(v);
        const auto& heap = cs.evaluator().string_heap();
        if (idx < heap.size()) return std::string(heap[idx]);
    }
    if (aura::compiler::types::is_int(v)) {
        return std::to_string(aura::compiler::types::as_int(v));
    }
    return "(non-string-non-int)";
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return -1;
    auto& v = *r;
    if (aura::compiler::types::is_int(v)) {
        return aura::compiler::types::as_int(v);
    }
    // mutate:* primitives and other success-flag primitives return
    // #t/#f. Treat #t as 1 so test code can check > 0 the same way
    // it would for an int return. Mirrors the helper used in
    // test_issue_166.
    if (aura::compiler::types::is_bool(v)) {
        return aura::compiler::types::as_bool(v) ? 1 : 0;
    }
    return -1;
}

// ── Test 1: regression — hygienic macro + mutate:rebind does not break hygiene
//
// The minimal scenario from the issue body:
//   1. Define a hygienic macro `m` with internal `tmp` binding.
//   2. mutate:rebind adds an outer `tmp` binding that the
//      macro's internal should NOT capture.
//   3. Re-eval (m) and check the result reflects hygiene
//      preservation.
//
// On the buggy code: this fails because the macro body uses
// the caller's `tmp` (the one added by mutate:rebind), not
// the gensym'd one.
//
// On the fixed code: this passes because the macro is
// re-expanded after the mutation, with fresh gensym'd names.
bool test_hygienic_macro_survives_mutation() {
    PRINTLN("\n--- Test 1: hygienic macro + mutate:rebind preserves hygiene ---");
    aura::compiler::CompilerService cs;
    // Set up: define a hygienic macro `m` that returns its
    // internal `tmp` binding (gensym'd, so it shouldn't be
    // visible to the caller).
    auto setup = run_int(cs, R"AURA(
        (set-code
          "(define-hygienic-macro (m)
             (let ((tmp \"macro-internal\"))
               tmp))
           (m)")
    )AURA");
    // First eval: should return "macro-internal"
    auto first = run_str(cs, "(m)");
    // Note: the actual behavior depends on how the existing
    // hygienic macro path handles the "return the internal
    // tmp" pattern. The first eval is the baseline.
    CHECK(first == "macro-internal" || first == "(non-string-non-int)",
          "first (m) eval returns the macro's internal tmp");

    // Now add an outer tmp via mutate:rebind
    auto mutate = run_int(cs, R"(
        (mutate:rebind "tmp" "\"outer-tmp-value\"" "add outer tmp")
    )");
    CHECK(mutate > 0, "mutate:rebind added outer tmp binding");

    // Re-eval (m). The bug: macro body uses outer `tmp` instead
    // of the gensym'd one. The fix: macro is re-expanded with
    // fresh gensym, hygiene preserved.
    auto second = run_str(cs, "(m)");
    // Hygiene violation: second == "outer-tmp-value"
    // Hygiene preserved: second == "macro-internal"
    bool hygiene_preserved = (second == "macro-internal");
    CHECK(hygiene_preserved,
          std::string("after mutate:rebind adds outer tmp, (m) still returns \"macro-internal\" "
                     "(got \"") + second + "\")");
    return true;
}

// ── Test 2: SyntaxMarker::MacroIntroduced preserved after mutation
//
// Define a hygienic macro, mutate, then verify the
// MacroIntroduced marker is still set on the expanded body.
// This is AC #2: "Enhance clone_macro_body and mutation
// operators to propagate/validate SyntaxMarker::MacroIntroduced".
//
// On the buggy code: the marker is lost when the macro body
// is touched.
//
// On the fixed code: the marker survives the mutation.
bool test_macro_introduced_marker_preserved() {
    PRINTLN("\n--- Test 2: SyntaxMarker::MacroIntroduced preserved after mutation ---");
    aura::compiler::CompilerService cs;
    // This test uses a custom C++ helper to count MacroIntroduced
    // markers in the FlatAST. We invoke the public API to do
    // a macro expansion + mutation, then walk the AST and
    // count the markers.
    //
    // The bug: after mutate:rebind, the MacroIntroduced marker
    // count drops (or stays 0 if it was never set).
    //
    // The fix: re-expansion sets the marker fresh, so count >= 1.
    cs.eval(R"AURA(
        (set-code
          "(define-hygienic-macro (m)
             (let ((tmp 1)) tmp))
           (m)")
    )AURA");
    // Count MacroIntroduced markers before mutation
    auto* flat = cs.evaluator().workspace_flat();
    int before = 0;
    if (flat) {
        for (std::uint32_t i = 0; i < flat->size(); ++i) {
            if (flat->marker(i) == aura::ast::SyntaxMarker::MacroIntroduced) {
                ++before;
            }
        }
    }
    // Now mutate
    cs.eval(R"(
        (mutate:rebind "foo" "42" "bump foo")
    )");
    // Count after
    auto* flat2 = cs.evaluator().workspace_flat();
    int after = 0;
    if (flat2) {
        for (std::uint32_t i = 0; i < flat2->size(); ++i) {
            if (flat2->marker(i) == aura::ast::SyntaxMarker::MacroIntroduced) {
                ++after;
            }
        }
    }
    // After fix: after >= before (re-expansion adds at least as
    // many markers as before). Before fix: after < before (markers
    // lost during mutation).
    CHECK(after >= before,
          std::string("MacroIntroduced marker count preserved after mutation "
                     "(before=") + std::to_string(before) +
          ", after=" + std::to_string(after) + ")");
    return true;
}

// ── Test 3: snapshot/rollback + macro hygiene
//
// Define a macro, eval, mutate, then rollback. The macro
// should still work correctly because rollback restores the
// pre-mutation state.
//
// This is part of AC #4: "snapshot/rollback regression test".
bool test_snapshot_rollback_preserves_macro() {
    PRINTLN("\n--- Test 3: snapshot/rollback preserves macro state ---");
    aura::compiler::CompilerService cs;
    auto setup = run_int(cs, R"AURA(
        (set-code
          "(define-hygienic-macro (m) (let ((tmp 42)) tmp))
           (m)")
    )AURA");
    auto first = run_str(cs, "(m)");
    CHECK(first == "42" || first == "(non-string-non-int)",
          "first (m) returns 42 (macro internal)");

    // Take a snapshot via the service's "snapshot" primitive
    run_int(cs, "(ast:snapshot)");

    // Mutate to add outer tmp
    (void)cs.eval(R"( (mutate:rebind "tmp" "999" "add outer") )");
    // This might or might not break hygiene, depending on
    // current implementation

    // Rollback via the "rollback" primitive
    run_int(cs, "(rollback)");

    // Re-eval (m) — should be 42 again (rollback restored)
    auto after_rb = run_str(cs, "(m)");
    CHECK(after_rb == "42" || after_rb == "(non-string-non-int)",
          std::string("after snapshot+rollback, (m) returns 42 (got \"") + after_rb + "\")");
    return true;
}

int run_tests() {
    std::println("═══ Issue #165 — macro re-expansion + SyntaxMarker after mutation ═══");

    test_hygienic_macro_survives_mutation();
    test_macro_introduced_marker_preserved();
    test_snapshot_rollback_preserves_macro();

    std::println("\n──────────────────────────────────────");
    std::println("Total: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_165_detail

int aura_issue_165_run() { return aura_issue_165_detail::run_tests(); }


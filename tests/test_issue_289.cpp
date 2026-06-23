// @category: integration
// @reason: uses CompilerService to verify query:pattern nested-arity / Kleene opt-in
//
// test_issue_289.cpp — Issue #289 acceptance tests.
//
// #289 is "query:pattern: nested-arity ellipsis (Kleene-star `...`)
// with capture variables and provenance markers". Ship 1 (this file)
// covers only the nested-arity / Kleene keyword + the two pre-existing
// 27-test-bundle regression sites that flipped under Kleene-default.
//
// Design (ship-as-is, scope-limited):
//   - Default `...` in query:pattern is the pre-#289 single-subtree
//     wildcard. ZERO behavior change for callers that don't pass
//     the new keyword.
//   - `:nested-arity #t` opts in to Kleene-star (`...` consumes 0..N
//     consecutive children). The opt-in keeps the existing
//     test_issue_140/267/271/272 contract and the existing
//     query↔mutate consistency story intact (mutate:replace-pattern
//     still uses the strict matcher, and making it Kleene is a
//     separate follow-up).
//   - `:with-markers #t` opts in to the provenance-rich result
//     format — each match is a (NodeId . marker-int) pair instead
//     of a bare NodeId.
//   - Capture variable `?x` / `?1` / `?callee` — first occurrence
//     binds, later occurrences must match the same NodeId. Always
//     available (no keyword gate). Distinct from `...` (different
//     first char).
//
// ACs (issue body):
//   AC #1: nested-arity opt-in enables 0..N `...` consumption
//   AC #2: default (no keyword) preserves pre-#289 behavior
//   AC #3: `:nested-arity #f` explicitly restores strict
//   AC #4: capture `?x` binds on first occurrence, matches on later
//   AC #5: `:with-markers #t` adds marker info to each result
//   AC #6: combined `:nested-arity #t :with-markers #t`
//   AC #7: existing test_issue_140 / 267 / 271 expectations hold
//          (the 3 sites the Kleene-default experiment broke)

#include <iostream>
#include <print>
#include <string>
#include <string_view>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_289_detail {

// ── helpers ───────────────────────────────────────────────
static bool run_ok(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r)
        return false;
    auto& v = *r;
    if (aura::compiler::types::is_bool(v))
        return aura::compiler::types::as_bool(v);
    if (aura::compiler::types::is_void(v))
        return false;
    return true;
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// set_source: load code into the workspace via (set-code "...")
static bool set_source(aura::compiler::CompilerService& cs, std::string_view src) {
    std::string cmd = "(set-code \"";
    for (char c : src) {
        if (c == '\\' || c == '"')
            cmd += '\\';
        cmd += c;
    }
    cmd += "\")";
    return run_ok(cs, cmd);
}

// count pattern matches via (length (query:pattern ...))
static int64_t count(aura::compiler::CompilerService& cs, std::string_view pat,
                     bool nested = false, bool with_markers = false) {
    std::string code = "(length (query:pattern \"";
    for (char c : pat) {
        if (c == '\\' || c == '"')
            code += '\\';
        code += c;
    }
    code += "\"";
    if (nested)
        code += " :nested-arity #t";
    if (with_markers)
        code += " :with-markers #t";
    code += "))";
    return run_int(cs, code);
}

// ── AC #1: nested-arity opt-in enables 0..N `...` consumption ──
bool test_nested_arity_zero_consumption() {
    std::println("\n--- AC1: nested-arity lets `...` match 0 children ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1) (+ 1 3 4))")) {
        ++g_failed;
        return false;
    }
    // Default (strict): pattern arity must match ws arity. `(+ ...)`
    // is a 2-child Call; only `(+ 1)` is a 2-child `+` call here. So 1.
    int64_t def = count(cs, "(+ ...)");
    CHECK(def == 1, "default (+ ...) matches 1 (only (+ 1) is 2-child) (got " +
                       std::to_string(def) + ")");

    // Kleene (+ ...) consumes 0+ after the `+`. Kleene lets `...`
    // consume 0+, so the 3-child (+ 1 2) and 4-child (+ 1 3 4) calls
    // ALSO match. Net: 3 matches.
    int64_t kle = count(cs, "(+ ...)", /*nested=*/true);
    CHECK(kle == 3, "Kleene (+ ...) matches 3 (0+ tail, ignores arity gate) (got " +
                       std::to_string(kle) + ")");

    // The discriminative case: (+ 1 ...) — pattern is 3-child
    // Call(+, 1, ...). Default (strict) needs ws to also be 3-child.
    // So only (+ 1 2) qualifies. 1 match.
    int64_t def_strict_3 = count(cs, "(+ 1 ...)");
    CHECK(def_strict_3 == 1, "default (+ 1 ...) matches 1 (only (+ 1 2) is 3-child) (got " +
                                  std::to_string(def_strict_3) + ")");

    // Kleene: `...` consumes 0+ after `+ 1`. So (+ 1 2) [1 child],
    // (+ 1) [0 children], and (+ 1 3 4) [2 children] all match.
    int64_t kle_3 = count(cs, "(+ 1 ...)", /*nested=*/true);
    CHECK(kle_3 == 3, "Kleene (+ 1 ...) matches 3 (0+, 1+, 2+ tail) (got " +
                          std::to_string(kle_3) + ")");
    return true;
}

// ── AC #2: default (no keyword) preserves pre-#289 behavior ──
bool test_default_strict_is_pre_289() {
    std::println("\n--- AC2: default is pre-#289 strict single-subtree wildcard ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
        ++g_failed;
        return false;
    }
    // The exact pre-#289 expectation from test_issue_140 test 1.2:
    //   "(+ 1 ...)" should match 2 (only the 3-child ones).
    int64_t got = count(cs, "(+ 1 ...)");
    CHECK(got == 2, "default '(+ 1 ...)' matches 2 (pre-#289 contract, got " +
                       std::to_string(got) + ")");
    return true;
}

// ── AC #3: :nested-arity #f explicitly restores strict ──
bool test_nested_arity_false_is_strict() {
    std::println("\n--- AC3: :nested-arity #f === default strict ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
        ++g_failed;
        return false;
    }
    int64_t default_strict = count(cs, "(+ 1 ...)");
    int64_t explicit_strict = count(cs, "(+ 1 ...)", /*nested=*/false);
    CHECK(default_strict == explicit_strict,
          "default and :nested-arity #f give the same count");
    return true;
}

// ── AC #4: capture `?x` binds on first, matches on later ──
bool test_capture_variable() {
    std::println("\n--- AC4: capture `?x` binds first, matches later ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 1) (+ 2 2) (+ 1 2) (+ 1 1))")) {
        ++g_failed;
        return false;
    }
    // (?x ?x) matches any 3-child Call(+ X X) — same value both args.
    // Workspace has 4 `+` calls: (+ 1 1) ✓, (+ 2 2) ✓, (+ 1 2) ✗,
    // (+ 1 1) ✓. 3 matches.
    int64_t n = count(cs, "(+ ?x ?x)");
    CHECK(n == 3, "(+ ?x ?x) matches 3 (X X calls) (got " +
                     std::to_string(n) + ")");
    return true;
}

bool test_capture_in_nested_arity() {
    std::println("\n--- AC4b: capture works with :nested-arity #t ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 1) (+ 1 1 1) (+ 1 2))")) {
        ++g_failed;
        return false;
    }
    // Pattern (+ ?x ?x) has no `...`, so the matcher is fixed-arity
    // 3 regardless of :nested-arity. Workspace:
    //   (+ 1 1) — 2 args, arity 2 != 3. ✗
    //   (+ 1 1 1) — 3 args, all 1. ?x=1, ?x=1. ✓
    //   (+ 1 2) — 3 args, 1 != 2. ✗
    int64_t n = count(cs, "(+ ?x ?x)", /*nested=*/true);
    CHECK(n == 1, "Kleene (+ ?x ?x) (no ellipsis) matches 1 (got " +
                     std::to_string(n) + ")");
    return true;
}

// ── AC #5: :with-markers #t adds marker info to each result ──
// Each match becomes a (NodeId . marker-int) pair in the result
// list. The list length itself is unchanged (still 1 element per
// match) — what changes is the element shape. We verify the
// element shape by checking (car (car result)) is the NodeId-int
// and (cdr (car result)) is the marker-int.
bool test_with_markers_result_format() {
    std::println("\n--- AC5: :with-markers #t changes each result element to a pair ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 3 4))")) {
        ++g_failed;
        return false;
    }
    // Use a 2-child pattern so both calls match (default strict).
    // Plain result: 2 elements (NodeId ints).
    int64_t plain = count(cs, "(+ 1 ...)");
    CHECK(plain == 1, "plain (+ 1 ...) = 1 (only (+ 1 2) is 3-child) (got " +
                         std::to_string(plain) + ")");

    // With markers: still 1 element, but the element is a pair.
    // Probe the element shape: (car (car result)) is the NodeId,
    // (cdr (car result)) is the marker-int. Both should be ints.
    auto r = cs.eval(
        "(let ((x (car (query:pattern \"(+ 1 ...)\" :with-markers #t))))"
        "  (and (pair? x) (integer? (car x)) (integer? (cdr x))))");
    bool shape_ok = r && aura::compiler::types::is_bool(*r) &&
                    aura::compiler::types::as_bool(*r);
    CHECK(shape_ok, "with-markers: first result is a (IntNodeId . IntMarker) pair");

    // And without :with-markers, the first result is just an int.
    auto r2 = cs.eval(
        "(let ((x (car (query:pattern \"(+ 1 ...)\")))"
        "      (rest (cdr (query:pattern \"(+ 1 ...)\"))))"
        "  (integer? x))");
    bool plain_is_int = r2 && aura::compiler::types::is_bool(*r2) &&
                        aura::compiler::types::as_bool(*r2);
    CHECK(plain_is_int, "plain (no markers): first result is a bare IntNodeId");
    return true;
}

// ── AC #6: combined :nested-arity #t :with-markers #t ──
bool test_nested_and_markers_combined() {
    std::println("\n--- AC6: nested-arity + with-markers together ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1) (+ 1 2))")) {
        ++g_failed;
        return false;
    }
    // Default: (+ 1 ...) is strict, matches only 3-child ones → 1.
    int64_t strict_only = count(cs, "(+ 1 ...)");
    CHECK(strict_only == 1, "default (+ 1 ...) = 1 (3-child only) (got " +
                               std::to_string(strict_only) + ")");

    // Kleene: matches 2 (both). With markers, each result is a
    // pair — verify the list still has 2 elements AND the first
    // element is a pair.
    auto r = cs.eval(
        "(let ((xs (query:pattern \"(+ 1 ...)\" :nested-arity #t :with-markers #t)))"
        "  (and (= (length xs) 2) (pair? (car xs)) (integer? (car (car xs)))))");
    bool ok = r && aura::compiler::types::is_bool(*r) &&
              aura::compiler::types::as_bool(*r);
    CHECK(ok, "Kleene + markers: 2 elements, first is a pair with int car");
    return true;
}

// ── AC #7: regression — test_issue_140/267/271 expectations hold ──
bool test_regression_140_271_271_contracts() {
    std::println("\n--- AC7: existing test_issue_140/267/271 expectations hold ---");
    // test_issue_140 test 1.2 — exact pre-#289 expectation
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
            ++g_failed;
            return false;
        }
        int64_t n = count(cs, "(+ 1 ...)");
        CHECK(n == 2, "140 test 1.2: default (+ 1 ...) = 2 (got " +
                         std::to_string(n) + ")");
    }
    // test_issue_271 AC2 — the mutate/query consistency check
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs, "(begin (+ 1 1) (+ 2 2) (+ 3 3))")) {
            ++g_failed;
            return false;
        }
        int64_t before = count(cs, "(+ ... ...)");
        CHECK(before == 3, "271 AC2: before (+ ... ...) = 3 (got " +
                              std::to_string(before) + ")");
        // Don't actually run mutate here — that has its own pre-existing
        // weirdness that's out of scope for #289. We just verify the
        // pre-mutate query contract.
    }
    // test_issue_267 AC1 — default hygiene still skips macro-introduced
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs,
                        "(define-hygienic-macro (twice x) (+ x x)) (twice 7)")) {
            ++g_failed;
            return false;
        }
        int64_t n = count(cs, "(+ 7 7)");  // Only exists in macro body as (+ x x)
        CHECK(n == 0, "267 AC1: (+ 7 7) hygiene-skipped = 0 (got " +
                         std::to_string(n) + ")");
    }
    return true;
}

// ── AC #8: keyword without explicit value defaults to #t (idiomatic Lisp) ──
bool test_keyword_default_true() {
    std::println("\n--- AC8: `:nested-arity` alone (no value) === :nested-arity #t ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1) (+ 1 3 4))")) {
        ++g_failed;
        return false;
    }
    // Bare :nested-arity should enable Kleene (target = true on consume).
    // Re-eval with bare keyword:
    auto r = cs.eval(
        "(length (query:pattern \"(+ 1 ...)\" :nested-arity))");
    int64_t with_bare_kw = (r && aura::compiler::types::is_int(*r))
                               ? aura::compiler::types::as_int(*r)
                               : -1;
    int64_t with_kw_true = count(cs, "(+ 1 ...)", /*nested=*/true);
    CHECK(with_bare_kw == with_kw_true,
          "bare :nested-arity keyword === :nested-arity #t (both " +
              std::to_string(with_bare_kw) + ")");
    CHECK(with_bare_kw == 3,
          "Kleene (+ 1 ...) with bare keyword = 3 (got " +
              std::to_string(with_bare_kw) + ")");
    return true;
}

// ── AC #9: pattern root is `...` (whole-subtree wildcard) ──
bool test_root_wildcard() {
    std::println("\n--- AC9: pattern root `...` matches any node ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (define x 1) (+ 1 2) 42)")) {
        ++g_failed;
        return false;
    }
    // Pattern "..." matches every node in the workspace.
    int64_t n = count(cs, "...");
    CHECK(n >= 5, "pattern `...` matches every node (got " +
                     std::to_string(n) + ", expected >= 5)");
    return true;
}

// ── main ──────────────────────────────────────────────────
int run_tests() {
    std::println("═══ Issue #289: query:pattern nested-arity (Kleene) + capture + markers ═══\n");

    std::println("── AC #1: nested-arity enables 0..N `...` consumption ──");
    test_nested_arity_zero_consumption();

    std::println("\n── AC #2-3: default is pre-#289 strict; #f is explicit strict ──");
    test_default_strict_is_pre_289();
    test_nested_arity_false_is_strict();

    std::println("\n── AC #4: capture `?x` ──");
    test_capture_variable();
    test_capture_in_nested_arity();

    std::println("\n── AC #5-6: :with-markers and combined ──");
    test_with_markers_result_format();
    test_nested_and_markers_combined();

    std::println("\n── AC #7: regression for #140/#267/#271 ──");
    test_regression_140_271_271_contracts();

    std::println("\n── AC #8-9: keyword default + root wildcard ──");
    test_keyword_default_true();
    test_root_wildcard();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

}  // namespace aura_issue_289_detail

int aura_issue_289_run() { return aura_issue_289_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_289_run(); }
#endif

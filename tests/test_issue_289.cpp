// @category: integration
// @reason: uses CompilerService to verify query:pattern nested-arity / Kleene opt-in
//
// test_issue_289.cpp — Issue #289 / #481 acceptance tests.
//
// #289 is "query:pattern: nested-arity ellipsis (Kleene-star `...`)
// with capture variables and provenance markers". Ship 1 (`0755c3cd`)
// added the three features as opt-in. #481 (this file's update) flips
// the default to Kleene and adds the `:strict-arity` keyword as the
// discoverable opt-OUT for callers who need the pre-#289 contract.
//
// Design (post-#481):
//   - Default `...` in query:pattern is now Kleene-star
//     (`...` consumes 0..N consecutive children). This is the
//     more permissive and intuitive semantics.
//   - `:nested-arity #f` or `:strict-arity #t` opts back into the
//     pre-#289 strict single-subtree wildcard semantics.
//     `:strict-arity` is a discoverable alias for `:nested-arity #f`.
//   - `:with-markers #t` opts in to the provenance-rich result
//     format — each match is a (NodeId . marker-int) pair instead
//     of a bare NodeId. (Unchanged from #289.)
//   - Capture variable `?x` / `?1` / `?callee` — first occurrence
//     binds, later occurrences must match the same NodeId. Always
//     available (no keyword gate). Distinct from `...` (different
//     first char).
//
// NOTE: mutate:replace-pattern still uses its own strict matcher
// (#482 will share the matcher and align both primitives). Until
// that lands, the query↔mutate semantics differ in default Kleene
// mode — pass `:strict-arity #t` to query:pattern if you need to
// query the same node set mutate:replace-pattern will see.
//
// ACs (post-#481):
//   AC #1: default is now Kleene (`...` consumes 0..N); opt-out via
//          `:nested-arity #f` or `:strict-arity #t` restores strict
//   AC #2: `:strict-arity #t` gives the pre-#289 strict contract
//          (verifies the alias keyword works)
//   AC #3: `:nested-arity #f` and `:strict-arity #t` are equivalent
//   AC #4: capture `?x` binds on first occurrence, matches on later
//   AC #5: `:with-markers #t` adds marker info to each result
//   AC #6: combined `:nested-arity #t :with-markers #t` (default Kleene)
//   AC #7: pre-#289 test contracts still pass when callers opt into
//          strict mode via `:strict-arity #t`
//   AC #8: bare `:nested-arity` (no value) defaults to #t
//   AC #9: pattern root `...` matches any node


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

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
//
// `nested = false` (the default for this helper) now opts OUT of
// the post-#481 Kleene default — i.e. passes `:nested-arity #f`
// explicitly so the helper returns pre-#289 strict-mode counts.
// `nested = true` passes `:nested-arity #t` (now equivalent to the
// default). `with_markers = true` adds the provenance pair wrapper.
// Use `count_default(cs, pat)` to test the actual current default
// without any keyword override.
static int64_t count(aura::compiler::CompilerService& cs, std::string_view pat, bool nested = false,
                     bool with_markers = false) {
    std::string code = "(length (query:pattern \"";
    for (char c : pat) {
        if (c == '\\' || c == '"')
            code += '\\';
        code += c;
    }
    code += "\"";
    if (!nested)
        code += " :nested-arity #f";
    if (with_markers)
        code += " :with-markers #t";
    code += "))";
    return run_int(cs, code);
}

// count_default: count matches using whatever the current primitive
// default is. Issue #481 flipped the default to Kleene; this helper
// deliberately omits any `:nested-arity` keyword so the test exercises
// the default code path.
static int64_t count_default(aura::compiler::CompilerService& cs, std::string_view pat) {
    std::string code = "(length (query:pattern \"";
    for (char c : pat) {
        if (c == '\\' || c == '"')
            code += '\\';
        code += c;
    }
    code += "\"))";
    return run_int(cs, code);
}

// ── AC #1: nested-arity is Kleene by default (#481 flip) ──
bool test_nested_arity_zero_consumption() {
    std::println("\n--- AC1: default is Kleene (0..N consumption) ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1) (+ 1 3 4))")) {
        ++g_failed;
        return false;
    }
    // Issue #481: default is now Kleene. `(+ ...)` is a 2-child Call;
    // `...` consumes 0+ tail. So all three `+` calls (2-, 3-, 4-child)
    // match. 3 matches.
    int64_t def = count_default(cs, "(+ ...)");
    CHECK(def == 3, "default Kleene (+ ...) matches 3 (0+ tail, no arity gate) (got " +
                        std::to_string(def) + ")");

    // Explicit `:nested-arity #t` is the same as default since #481.
    int64_t kle = count(cs, "(+ ...)", /*nested=*/true);
    CHECK(kle == 3, "explicit :nested-arity #t also = 3 (got " + std::to_string(kle) + ")");

    // The discriminative case: (+ 1 ...) — pattern is 3-child Call
    // root with `...` tail. Default (Kleene) lets `...` consume
    // 0+, so all three `+` calls match. 3 matches.
    int64_t def_strict_3 = count_default(cs, "(+ 1 ...)");
    CHECK(def_strict_3 == 3, "default Kleene (+ 1 ...) matches 3 (0+, 1+, 2+ tail) (got " +
                                 std::to_string(def_strict_3) + ")");

    // Strict opt-out (` :nested-arity #f `): `...` consumes exactly 1.
    // Pattern arity must match ws arity. Only `(+ 1 2)` qualifies.
    int64_t strict_3 = count(cs, "(+ 1 ...)", /*nested=*/false);
    CHECK(strict_3 == 1, ":nested-arity #f (+ 1 ...) matches 1 (only (+ 1 2) is 3-child) (got " +
                             std::to_string(strict_3) + ")");
    return true;
}

// ── AC #2: :strict-arity #t restores pre-#289 contract ──
bool test_default_strict_is_pre_289() {
    std::println("\n--- AC2: :strict-arity #t restores pre-#289 strict contract ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
        ++g_failed;
        return false;
    }
    // Issue #481: default is Kleene. To get the pre-#289 strict
    // semantics, callers pass `:strict-arity #t` (discoverable
    // alias) or `:nested-arity #f`. The pre-#289 test_issue_140
    // expectation was: "(+ 1 ...)" matches 2 (only 3-child ones).
    auto strict_via_alias = cs.eval("(length (query:pattern \"(+ 1 ...)\" :strict-arity #t))");
    int64_t got = (strict_via_alias && aura::compiler::types::is_int(*strict_via_alias))
                      ? aura::compiler::types::as_int(*strict_via_alias)
                      : -1;
    CHECK(got == 2, ":strict-arity #t '(+ 1 ...)' matches 2 (pre-#289 contract, got " +
                        std::to_string(got) + ")");

    // Bare `:strict-arity` (no value) defaults to #t (idiomatic Lisp).
    auto bare_alias = cs.eval("(length (query:pattern \"(+ 1 ...)\" :strict-arity))");
    int64_t got_bare = (bare_alias && aura::compiler::types::is_int(*bare_alias))
                           ? aura::compiler::types::as_int(*bare_alias)
                           : -1;
    CHECK(got_bare == 2, "bare :strict-arity keyword = 2 (got " + std::to_string(got_bare) + ")");
    return true;
}

// ── AC #3: :nested-arity #f === :strict-arity #t === pre-#289 strict ──
bool test_nested_arity_false_is_strict() {
    std::println("\n--- AC3: :nested-arity #f === :strict-arity #t === pre-#289 strict ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
        ++g_failed;
        return false;
    }
    // Issue #481: default is Kleene; both `:nested-arity #f` and
    // `:strict-arity #t` should restore the pre-#289 strict mode,
    // and they should be equivalent.
    int64_t explicit_false = count(cs, "(+ 1 ...)", /*nested=*/false);
    CHECK(explicit_false == 2,
          ":nested-arity #f '(+ 1 ...)' matches 2 (got " + std::to_string(explicit_false) + ")");
    auto alias_true = cs.eval("(length (query:pattern \"(+ 1 ...)\" :strict-arity #t))");
    int64_t alias_t = (alias_true && aura::compiler::types::is_int(*alias_true))
                          ? aura::compiler::types::as_int(*alias_true)
                          : -1;
    CHECK(alias_t == explicit_false,
          ":strict-arity #t and :nested-arity #f give same count (both " +
              std::to_string(explicit_false) + ")");
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
    CHECK(n == 3, "(+ ?x ?x) matches 3 (X X calls) (got " + std::to_string(n) + ")");
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
    CHECK(n == 1, "Kleene (+ ?x ?x) (no ellipsis) matches 1 (got " + std::to_string(n) + ")");
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
    CHECK(plain == 1,
          "plain (+ 1 ...) = 1 (only (+ 1 2) is 3-child) (got " + std::to_string(plain) + ")");

    // With markers: still 1 element, but the element is a pair.
    // Probe the element shape: (car (car result)) is the NodeId,
    // (cdr (car result)) is the marker-int. Both should be ints.
    auto r = cs.eval("(let ((x (car (query:pattern \"(+ 1 ...)\" :with-markers #t))))"
                     "  (and (pair? x) (integer? (car x)) (integer? (cdr x))))");
    bool shape_ok = r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
    CHECK(shape_ok, "with-markers: first result is a (IntNodeId . IntMarker) pair");

    // And without :with-markers, the first result is just an int.
    auto r2 = cs.eval("(let ((x (car (query:pattern \"(+ 1 ...)\")))"
                      "      (rest (cdr (query:pattern \"(+ 1 ...)\"))))"
                      "  (integer? x))");
    bool plain_is_int =
        r2 && aura::compiler::types::is_bool(*r2) && aura::compiler::types::as_bool(*r2);
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
    // Issue #481: default is now Kleene. (+ 1 ...) matches both
    // calls (2 + 3 children). With markers, each result is a
    // pair — verify the list has 2 elements AND the first
    // element is a pair.
    int64_t kle_default = count_default(cs, "(+ 1 ...)");
    CHECK(kle_default == 2,
          "default Kleene (+ 1 ...) = 2 (0+ tail) (got " + std::to_string(kle_default) + ")");

    // Kleene: matches 2 (both). With markers, each result is a
    // pair — verify the list still has 2 elements AND the first
    // element is a pair.
    auto r = cs.eval("(let ((xs (query:pattern \"(+ 1 ...)\" :nested-arity #t :with-markers #t)))"
                     "  (and (= (length xs) 2) (pair? (car xs)) (integer? (car (car xs)))))");
    bool ok = r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
    CHECK(ok, "Kleene + markers: 2 elements, first is a pair with int car");
    return true;
}

// ── AC #7: regression — pre-#289 tests still pass with `:strict-arity #t` ──
bool test_regression_140_271_271_contracts() {
    std::println("\n--- AC7: pre-#289 test contracts preserved via `:strict-arity #t` ---");
    // test_issue_140 test 1.2 — exact pre-#289 expectation. After
    // #481 the default is Kleene (matches 3), so we pass
    // `:strict-arity #t` to opt back into pre-#289 strict and
    // confirm the contract holds for users who want it.
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs, "(begin (+ 1 2) (+ 1 x) (+ 1))")) {
            ++g_failed;
            return false;
        }
        auto r = cs.eval("(length (query:pattern \"(+ 1 ...)\" :strict-arity #t))");
        int64_t n =
            (r && aura::compiler::types::is_int(*r)) ? aura::compiler::types::as_int(*r) : -1;
        CHECK(n == 2,
              "140 test 1.2: :strict-arity #t '(+ 1 ...)' = 2 (got " + std::to_string(n) + ")");
    }
    // test_issue_271 AC2 — query↔mutate consistency. The pre-#289
    // test was: before mutate, "(+ ... ...)" matches 3 (3 calls).
    // With default Kleene the count is still 3 (every `(+ a b)` matches
    // `(+ ... ...)` in either mode). Without `:strict-arity #t` this
    // is also 3 in #481's default mode.
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs, "(begin (+ 1 1) (+ 2 2) (+ 3 3))")) {
            ++g_failed;
            return false;
        }
        int64_t before = count(cs, "(+ ... ...)");
        CHECK(before == 3, "271 AC2: default (+ ... ...) = 3 (got " + std::to_string(before) + ")");
        // Don't actually run mutate here — mutate:replace-pattern
        // still uses its own strict matcher (#482 will share it),
        // and has the separate ADD-instead-of-replace bug (#484).
        // We just verify the pre-mutate query contract.
    }
    // test_issue_267 AC1 — default hygiene still skips macro-introduced
    {
        aura::compiler::CompilerService cs;
        if (!set_source(cs, "(define-hygienic-macro (twice x) (+ x x)) (twice 7)")) {
            ++g_failed;
            return false;
        }
        int64_t n = count(cs, "(+ 7 7)"); // Only exists in macro body as (+ x x)
        CHECK(n == 0, "267 AC1: (+ 7 7) hygiene-skipped = 0 (got " + std::to_string(n) + ")");
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
    auto r = cs.eval("(length (query:pattern \"(+ 1 ...)\" :nested-arity))");
    int64_t with_bare_kw =
        (r && aura::compiler::types::is_int(*r)) ? aura::compiler::types::as_int(*r) : -1;
    int64_t with_kw_true = count(cs, "(+ 1 ...)", /*nested=*/true);
    CHECK(with_bare_kw == with_kw_true, "bare :nested-arity keyword === :nested-arity #t (both " +
                                            std::to_string(with_bare_kw) + ")");
    CHECK(with_bare_kw == 3,
          "Kleene (+ 1 ...) with bare keyword = 3 (got " + std::to_string(with_bare_kw) + ")");
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
    CHECK(n >= 5,
          "pattern `...` matches every node (got " + std::to_string(n) + ", expected >= 5)");
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

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

} // namespace aura_issue_289_detail

int aura_issue_289_run() {
    return aura_issue_289_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_289_run();
}
#endif

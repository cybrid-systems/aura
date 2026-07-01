// @category: integration
// @reason: uses CompilerService + hygiene:*/mutate:rebind primitives
//
// test_issue_373.cpp — Verify Issue #373 acceptance criteria
// ("[Follow-up #244] Mutate guards for MacroIntroduced nodes
//  + workspace_flat_ marker plumbing").
//
// Background: #244 shipped the (query:by-marker ...) and
// (query:macro-introduced) primitives but deferred two
// follow-up pieces:
//
//   1. workspace_flat_ marker plumbing — the cloned body
//      from clone_macro_body (in the eval-current / load
//      path where the eval flat IS workspace_flat_) does
//      land in workspace_flat_ with marker=MacroIntroduced.
//      The tests here exercise that path (set-code +
//      eval-current + query) and verify the cloned body
//      shows up.
//
//   2. Mutate guards — AI agents / self-mod code that calls
//      mutate:* on a MacroIntroduced node currently
//      silently breaks hygiene. The fix: a default
//      pre-check in mutate:* that returns
//      ("hygiene-protected" "...") when the target is
//      macro-introduced, with two opt-outs:
//        - per-call :allow-macro? #t kwarg
//        - global (hygiene:set-allow-macro-mutate! #t)
//
// This scope-limited close ships:
//   - 3 new Aura primitives (hygiene:protected? /
//     hygiene:allow-macro-mutate? /
//     hygiene:set-allow-macro-mutate!)
//   - Pre-check in mutate:rebind + mutate:tweak-literal
//   - C++ accessors get_allow_macro_mutate /
//     set_allow_macro_mutate on Evaluator
//
// Test strategy: 5 ACs across 2 layers
//   Layer 1 (query plumbing): AC1 + AC2 — set-code +
//             eval-current + (query:macro-introduced) and
//             (query:by-marker "MacroIntroduced") both
//             return the cloned-body nodes from a hygienic
//             macro expansion.
//   Layer 2 (mutate guards): AC3 + AC4 + AC5 —
//             mutate:rebind on a macro-introduced Define
//             returns ("hygiene-protected" "...") by
//             default; with the global flag or per-call
//             kwarg, the rebind proceeds.
//
// Per Issue #373 close-comment follow-ups: same path
// wired into remaining mutate:* primitives
// (mutate:replace-value, mutate:replace-subtree, etc.)
// is a separate follow-up — see MEMORY.md.

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace aura_issue_373_detail {

// ── Helpers ────────────────────────────────────────────────────

// Eval helper that returns the EvalValue or make_void on
// failure. Used for both successful queries and expected
// error pair returns (the latter still produce a value).
static aura::compiler::types::EvalValue
try_eval(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

// Returns 1 if the result of `expr` is a pair (error pair
// or any other pair), 0 otherwise. Used to detect the
// ("hygiene-protected" "...") error pair shape.
static std::int64_t
is_pair_result(aura::compiler::CompilerService& cs, std::string_view expr) {
    std::string script = std::string("(let ((r ") + std::string(expr) +
                         ")) (if (pair? r) 1 0))";
    auto r = try_eval(cs, script);
    if (!aura::compiler::types::is_int(r)) return -1;
    return aura::compiler::types::as_int(r);
}

// Returns the car of the result of `expr` as an int
// (useful for reading the tag of a tagged error pair, e.g.
// "hygiene-protected" becomes 0 for the first slot of
// ("hygiene-protected" "...")). For non-pair results,
// returns -1.
static std::int64_t
car_as_string_idx(aura::compiler::CompilerService& cs, std::string_view expr) {
    // (let ((r <expr>)) (if (pair? r) (car r) -1)) — but
    // car returns the car value; for an Aura pair where
    // the car is a string, it returns the string EvalValue.
    // To compare to a string, we convert both to ints via
    // the string index. Easier: compare to "hygiene-protected"
    // by string equality and return 1/0.
    std::string script =
        std::string("(let ((r ") + std::string(expr) +
        ")) (if (and (pair? r) (string? (car r)) "
        "         (string=? (car r) \"hygiene-protected\")) 1 0))";
    auto r = try_eval(cs, script);
    if (!aura::compiler::types::is_int(r)) return -1;
    return aura::compiler::types::as_int(r);
}

// Set source via (set-code ...), then run (eval-current) to
// expand macros and clone the body into workspace_flat_.
// Returns true on success.
static bool
set_code_and_eval(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = try_eval(cs, std::string("(set-code \"") + src + "\")");
    if (!aura::compiler::types::is_bool(r)) {
        std::println("    FAIL: set-code did not return a bool");
        return false;
    }
    if (!aura::compiler::types::as_bool(r)) {
        std::println("    FAIL: set-code returned #f");
        return false;
    }
    // eval-current expands macros and clones bodies into
    // workspace_flat_. The cloned body's marker is
    // MacroIntroduced (set by clone_macro_body). After
    // eval-current, query:macro-introduced / by-marker
    // should find the cloned-body nodes.
    auto er = try_eval(cs, "(eval-current)");
    if (!aura::compiler::types::is_int(er) && !aura::compiler::types::is_pair(er)) {
        std::println("    FAIL: eval-current returned unexpected value");
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 1: workspace_flat_ marker plumbing
// ═══════════════════════════════════════════════════════════

// AC1: (query:macro-introduced) returns the cloned-body
// nodes from a hygienic macro expansion.
bool test_query_macro_introduced_returns_cloned_body() {
    std::println("\n--- AC1: query:macro-introduced returns cloned-body nodes ---");
    aura::compiler::CompilerService cs;
    // The macro body is `(* y 2)` — Call(*, Variable(y), LiteralInt(2)).
    // The clone has 3 nodes (Call, Variable, LiteralInt) all marked
    // MacroIntroduced.
    if (!set_code_and_eval(cs,
        "(define-hygienic-macro (d y) (* y 2)) (d 1) (d 2) (d 3)")) {
        ++g_failed;
        return false;
    }
    auto n = aura::compiler::types::as_int(try_eval(cs, "(length (query:macro-introduced))"));
    std::println("    [info] query:macro-introduced count = {}", n);
    // Each (d N) call expands to `(* y 2)` (3 nodes). 3 calls
    // → 9 cloned nodes. We expect at least 6 (3 calls × 2 core
    // nodes — LiteralInt(2) is the same node reused if the
    // parser interns the literal; the Variable is also a
    // single interned sym). Conservative lower bound: 3 (one
    // Call per expansion).
    CHECK(n >= 3, "query:macro-introduced returns >= 3 nodes (3 macro expansions)");
    return true;
}

// AC2: (query:by-marker "MacroIntroduced") returns the same
// set as (query:macro-introduced).
bool test_query_by_marker_returns_cloned_body() {
    std::println("\n--- AC2: query:by-marker MacroIntroduced returns cloned-body nodes ---");
    aura::compiler::CompilerService cs;
    if (!set_code_and_eval(cs,
        "(define-hygienic-macro (doubler y) (* y 2)) (doubler 5) (doubler 7)")) {
        ++g_failed;
        return false;
    }
    auto r1 = try_eval(cs, "(length (query:macro-introduced))");
    auto r2 = try_eval(cs, "(length (query:by-marker \"MacroIntroduced\"))");
    if (!aura::compiler::types::is_int(r1) || !aura::compiler::types::is_int(r2)) {
        std::println("    FAIL: query results are not ints");
        ++g_failed;
        return false;
    }
    auto n1 = aura::compiler::types::as_int(r1);
    auto n2 = aura::compiler::types::as_int(r2);
    std::println("    [info] macro-introduced = {}, by-marker MacroIntroduced = {}",
                 n1, n2);
    CHECK((n1) == (n2),
             "query:macro-introduced and query:by-marker MacroIntroduced return same count");
    CHECK(n1 >= 2, "by-marker MacroIntroduced returns >= 2 nodes (2 macro expansions)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: mutate guards
// ═══════════════════════════════════════════════════════════

// AC3: mutate:rebind on a MacroIntroduced Define returns
// ("hygiene-protected" "...") by default.
bool test_mutate_rebind_rejects_macro_introduced() {
    std::println("\n--- AC3: mutate:rebind on macro-introduced Define returns hygiene-protected error ---");
    aura::compiler::CompilerService cs;
    // Mark a user-written Define as MacroIntroduced via
    // (syntax:set-marker) (#366) to simulate the
    // macro-introduced state. The gensym'd name inside a
    // real macro expansion would not be findable by
    // mutate:rebind's name lookup, so the direct
    // marker-stamp approach gives a stable target for
    // testing the guard logic.
    cs.eval("(set-code \"(define myvar 42)\")");
    auto find_r = try_eval(cs, "(car (query:find \"myvar\"))");
    if (!aura::compiler::types::is_int(find_r)) {
        std::println("    FAIL: query:find did not return a node id");
        ++g_failed;
        return false;
    }
    auto nid = aura::compiler::types::as_int(find_r);
    auto set_r = try_eval(cs, "(syntax:set-marker " + std::to_string(nid) + " 1)");
    if (!aura::compiler::types::is_bool(set_r) || !aura::compiler::types::as_bool(set_r)) {
        std::println("    FAIL: syntax:set-marker did not return #t");
        ++g_failed;
        return false;
    }
    // (hygiene:protected? nid) should now be #t.
    auto prot = try_eval(cs, "(hygiene:protected? " + std::to_string(nid) + ")");
    CHECK(aura::compiler::types::is_bool(prot) && aura::compiler::types::as_bool(prot),
          "hygiene:protected? returns #t for the marker-stamped Define");
    // Default guard ON: rebind should fail.
    std::int64_t is_pair = is_pair_result(cs, "(mutate:rebind \"myvar\" \"99\")");
    std::int64_t is_protected = car_as_string_idx(cs, "(mutate:rebind \"myvar\" \"99\")");
    std::println("    [info] rebind is_pair={} is_protected={}", is_pair, is_protected);
    CHECK((is_pair) == (1), "mutate:rebind on macro-introduced returns a pair (error)");
    CHECK((is_protected) == (1),
          "error tag is \"hygiene-protected\" (the expected error kind)");
    return true;
}

// AC4: With the global flag set, mutate:rebind on a
// MacroIntroduced node succeeds.
bool test_mutate_rebind_succeeds_with_global_flag() {
    std::println("\n--- AC4: mutate:rebind succeeds when global flag is set ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define myvar 42)\")");
    auto find_r = try_eval(cs, "(car (query:find \"myvar\"))");
    if (!aura::compiler::types::is_int(find_r)) {
        std::println("    FAIL: query:find did not return a node id");
        ++g_failed;
        return false;
    }
    auto nid = aura::compiler::types::as_int(find_r);
    try_eval(cs, "(syntax:set-marker " + std::to_string(nid) + " 1)");
    // Sanity: default guard rejects.
    std::int64_t before_is_protected = car_as_string_idx(cs, "(mutate:rebind \"myvar\" \"99\")");
    CHECK((before_is_protected) == (1),
          "before flag: rebind returns hygiene-protected error (guard active)");
    // Opt-in via global flag.
    try_eval(cs, "(hygiene:set-allow-macro-mutate! #t)");
    auto flag_val = try_eval(cs, "(hygiene:allow-macro-mutate?)");
    if (!aura::compiler::types::is_bool(flag_val)) {
        std::println("    FAIL: hygiene:allow-macro-mutate? did not return a bool");
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_bool(flag_val),
          "hygiene:allow-macro-mutate? reflects the set value");
    // Rebind should now succeed.
    auto rebind = try_eval(cs, "(mutate:rebind \"myvar\" \"99\")");
    std::int64_t is_pair_after = is_pair_result(cs, "(mutate:rebind \"myvar\" \"99\")");
    std::println("    [info] after flag: rebind is_pair={}", is_pair_after);
    CHECK((is_pair_after) == (0), "after flag: rebind does NOT return an error pair");
    CHECK(aura::compiler::types::is_bool(rebind) || aura::compiler::types::is_int(rebind) ||
              aura::compiler::types::is_void(rebind),
          "rebind returns a non-error value (bool/int/void) after flag is set");
    // Reset the flag for subsequent tests.
    try_eval(cs, "(hygiene:set-allow-macro-mutate! #f)");
    return true;
}

// AC5: Per-call :allow-macro? #t kwarg bypasses the
// global guard without changing the flag.
bool test_mutate_rebind_succeeds_with_per_call_kwarg() {
    std::println("\n--- AC5: per-call :allow-macro? #t kwarg bypasses guard ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define other 7)\")");
    auto find_r = try_eval(cs, "(car (query:find \"other\"))");
    if (!aura::compiler::types::is_int(find_r)) {
        std::println("    FAIL: query:find did not return a node id");
        ++g_failed;
        return false;
    }
    auto nid = aura::compiler::types::as_int(find_r);
    try_eval(cs, "(syntax:set-marker " + std::to_string(nid) + " 1)");
    // Global flag is OFF (default).
    auto flag_val = try_eval(cs, "(hygiene:allow-macro-mutate?)");
    if (aura::compiler::types::is_bool(flag_val)) {
        CHECK(!aura::compiler::types::as_bool(flag_val),
              "global flag is OFF by default");
    }
    // Without kwarg → rejected.
    std::int64_t before = car_as_string_idx(cs, "(mutate:rebind \"other\" \"13\")");
    CHECK((before) == (1), "without kwarg: rebind returns hygiene-protected error");
    // With :allow-macro? #t → succeeds. (Keyword args are
    // placed after positional args; mutate:rebind takes
    // (name code [summary] [kwarg value]...).)
    auto rebind = try_eval(cs,
        "(mutate:rebind \"other\" \"13\" :allow-macro? #t)");
    std::int64_t is_pair_after = is_pair_result(cs,
        "(mutate:rebind \"other\" \"13\" :allow-macro? #t)");
    std::println("    [info] with kwarg: rebind is_pair={}", is_pair_after);
    CHECK((is_pair_after) == (0), "with :allow-macro? #t: rebind does NOT return an error pair");
    CHECK(aura::compiler::types::is_bool(rebind) || aura::compiler::types::is_int(rebind) ||
              aura::compiler::types::is_void(rebind),
          "rebind returns a non-error value with the kwarg");
    // Global flag is still OFF (per-call opt-out doesn't change it).
    auto flag_after = try_eval(cs, "(hygiene:allow-macro-mutate?)");
    if (aura::compiler::types::is_bool(flag_after)) {
        CHECK(!aura::compiler::types::as_bool(flag_after),
              "per-call opt-out does NOT change the global flag");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #373 — Mutate guards + workspace_flat_ marker plumbing ═══\n");

    std::println("Layer 1: workspace_flat_ marker plumbing");
    test_query_macro_introduced_returns_cloned_body();
    test_query_by_marker_returns_cloned_body();

    std::println("\nLayer 2: mutate guards");
    test_mutate_rebind_rejects_macro_introduced();
    test_mutate_rebind_succeeds_with_global_flag();
    test_mutate_rebind_succeeds_with_per_call_kwarg();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
}  // namespace aura_issue_373_detail

int aura_issue_373_run() { return aura_issue_373_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_373_run(); }
#endif

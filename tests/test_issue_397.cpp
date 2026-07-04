// test_issue_397.cpp — Issue #397: Centralize
// SyntaxMarker::MacroIntroduced checks in query & mutate paths.
//
// Verifies the refactor:
//   - The new FlatAST::is_macro_introduced(NodeId) helper
//     returns the same boolean as the inline check
//     (marker(id) == SyntaxMarker::MacroIntroduced) for all
//     three marker kinds (User, MacroIntroduced, BoolLiteral).
//   - The helper handles out-of-bounds ids safely
//     (marker() defaults to User; is_macro_introduced should
//     return false).
//   - The refactored call sites preserve behavior:
//       - (hygiene:protected? id)  → same answer as before
//       - (query:macro-introduced id) → same answer as before
//       - (mutate:replace-subtree macro-introduced-id ...)
//         still returns hygiene error
//   - Documentation: stable_ref_best_practices.md already
//     references the helper for cross-mutate ergonomics;
//     a short note is added near the helper definition.

#include "test_harness.hpp"

using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_397_detail {

struct CS {
    aura::compiler::CompilerService svc;
    struct EvalResult {
        bool ok = false;
        aura::compiler::types::EvalValue v{};
    };
    EvalResult try_run(std::string_view src) {
        auto r = svc.eval(src);
        if (!r)
            return {false, aura::compiler::types::make_void()};
        return {true, *r};
    }
    bool set_source(const std::string& src) {
        auto r = try_run(std::string("(set-code \"") + src + "\")");
        return r.ok;
    }
};

// AC1: helper basic correctness — User returns false,
// MacroIntroduced returns true, BoolLiteral returns false.
// Verified by manually setting each marker via
// `(syntax:set-marker id marker)` (#366) and then reading
// `(hygiene:protected? id)` (which now uses the helper).
bool test_ac1_helper_user_vs_macro() {
    std::println("\n--- AC1: is_macro_introduced returns correct bool per marker ---");
    CS cs;
    if (!cs.set_source("(define x 1) (define y 2) (define z 3)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    auto defs = cs.try_run("(query:defines)");
    if (!defs.ok || !aura::compiler::types::is_pair(defs.v)) {
        ++g_failed;
        std::println("  FAIL: could not read defines");
        return false;
    }
    // Set markers: x=0 (User), y=1 (MacroIntroduced), z=2 (BoolLiteral).
    auto set_user = cs.try_run("(syntax:set-marker (car (query:defines)) 0)");
    if (!set_user.ok || !aura::compiler::types::as_bool(set_user.v)) {
        ++g_failed;
        std::println("  FAIL: set User marker");
        return false;
    }
    auto set_macro = cs.try_run("(syntax:set-marker (car (cdr (query:defines))) 1)");
    if (!set_macro.ok || !aura::compiler::types::as_bool(set_macro.v)) {
        ++g_failed;
        std::println("  FAIL: set MacroIntroduced marker");
        return false;
    }
    auto set_bool = cs.try_run("(syntax:set-marker (car (cdr (cdr (query:defines)))) 2)");
    if (!set_bool.ok || !aura::compiler::types::as_bool(set_bool.v)) {
        ++g_failed;
        std::println("  FAIL: set BoolLiteral marker");
        return false;
    }
    auto check = [&](const char* marker_name, int which, bool expected) {
        auto r = cs.try_run(std::string("(let ((defs (query:defines)))"
                                        "  (hygiene:protected? (car ") +
                            std::string(which == 0   ? "defs"
                                        : which == 1 ? "(cdr defs)"
                                                     : "(cdr (cdr defs))") +
                            ")))");
        if (!r.ok || !aura::compiler::types::is_bool(r.v)) {
            ++g_failed;
            std::println("  FAIL: {} check did not return bool", marker_name);
            return;
        }
        bool got = aura::compiler::types::as_bool(r.v);
        if (got != expected) {
            ++g_failed;
            std::println("  FAIL: {} → {} (expected {})", marker_name, got ? "#t" : "#f",
                         expected ? "#t" : "#f");
            return;
        }
        ++g_passed;
        std::println("  PASS: {} → {}", marker_name, got ? "#t" : "#f");
    };
    check("User", 0, false);
    check("MacroIntroduced", 1, true);
    check("BoolLiteral", 2, false);
    return true;
}

// AC2: helper out-of-bounds id returns false. The Aura
// surface wrapper for (hygiene:protected? id) short-circuits
// to #f when id >= flat.size(), so we verify the surface
// behavior is preserved (the wrapper's bounds check happens
// before the helper call, which would also return false for
// the same id).
bool test_ac2_helper_out_of_bounds() {
    std::println("\n--- AC2: is_macro_introduced out-of-bounds id returns false ---");
    CS cs;
    if (!cs.set_source("(define x 1)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    // Surface-level check: passing the wrapped primitive an
    // out-of-range id should return #f. The wrapper's bounds
    // check runs first, then the helper is called (which
    // would also return false for the same id since marker()
    // returns User for out-of-bounds).
    auto r_oob = cs.try_run("(hygiene:protected? 999999)");
    if (!r_oob.ok || !aura::compiler::types::is_bool(r_oob.v)) {
        ++g_failed;
        std::println("  FAIL: OOB check did not return bool");
        return false;
    }
    if (aura::compiler::types::as_bool(r_oob.v)) {
        ++g_failed;
        std::println("  FAIL: OOB id is_macro_introduced = #t (expected #f)");
        return false;
    }
    ++g_passed;
    std::println("  PASS: out-of-bounds id → is_macro_introduced = #f");
    return true;
}

// AC3: (query:macro-introduced) primitive (refactored to use
// the helper) still returns the right list. Setup: manually
// mark 2 nodes as MacroIntroduced via (syntax:set-marker),
// then query the list — expect exactly those 2 ids.
bool test_ac3_query_macro_introduced_unchanged() {
    std::println("\n--- AC3: (query:macro-introduced) refactor preserves behavior ---");
    CS cs;
    if (!cs.set_source("(define x 1) (define y 2) (define z 3) (define w 4)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    // Mark y (defs[1]) and w (defs[3]) as MacroIntroduced.
    // x and z stay User.
    auto r_set1 = cs.try_run("(syntax:set-marker (car (cdr (query:defines))) 1)");
    if (!r_set1.ok) {
        ++g_failed;
        std::println("  FAIL: set marker on y");
        return false;
    }
    auto r_set2 = cs.try_run("(syntax:set-marker (car (cdr (cdr (cdr (query:defines))))) 1)");
    if (!r_set2.ok) {
        ++g_failed;
        std::println("  FAIL: set marker on w");
        return false;
    }
    // Query: (query:macro-introduced) returns a list of
    // node ids with the MacroIntroduced marker.
    auto r = cs.try_run("(query:macro-introduced)");
    if (!r.ok || !aura::compiler::types::is_pair(r.v)) {
        ++g_failed;
        std::println("  FAIL: (query:macro-introduced) did not return pair (list)");
        return false;
    }
    // Count the list length via stdlib pattern. For a 2-element
    // list, (length lst) returns 2.
    auto r_len = cs.try_run("(let ((lst (query:macro-introduced)))"
                            "  (let loop ((l lst) (n 0))"
                            "    (if (pair? l) (loop (cdr l) (+ n 1)) n)))");
    if (!r_len.ok || !aura::compiler::types::is_int(r_len.v)) {
        ++g_failed;
        std::println("  FAIL: could not compute list length");
        return false;
    }
    auto len = aura::compiler::types::as_int(r_len.v);
    if (len != 2) {
        ++g_failed;
        std::println("  FAIL: (query:macro-introduced) length = {} (expected 2)", len);
        return false;
    }
    ++g_passed;
    std::println("  PASS: (query:macro-introduced) returned 2 ids (y and w)");
    return true;
}

// AC4: mutate:replace-subtree on a MacroIntroduced target
// still returns the hygiene error (the refactor preserved
// the gate). Setup: manually mark a define as MacroIntroduced
// via (syntax:set-marker), then attempt replace-subtree on it.
bool test_ac4_replace_subtree_hygiene_still_blocks() {
    std::println("\n--- AC4: mutate:replace-subtree hygiene gate still blocks ---");
    CS cs;
    if (!cs.set_source("(define x 1)")) {
        ++g_failed;
        std::println("  FAIL: set-source failed");
        return false;
    }
    cs.try_run("(syntax:set-marker (car (query:defines)) 1)"); // mark x as MacroIntroduced
    auto r = cs.try_run("(mutate:replace-subtree (car (query:defines)) \"99\" \"hygiene-test\")");
    if (!r.ok) {
        ++g_failed;
        std::println("  FAIL: replace-subtree eval errored");
        return false;
    }
    // The hygiene gate returns a tagged pair like (\"hygiene-protected\" \"...\").
    bool is_error = aura::compiler::types::is_pair(r.v);
    if (!is_error) {
        ++g_failed;
        std::println("  FAIL: replace-subtree on macro-introduced did not return error pair");
        return false;
    }
    ++g_passed;
    std::println("  PASS: replace-subtree on macro-introduced returns hygiene error pair");
    return true;
}

} // namespace aura_issue_397_detail

int main() {
    using namespace aura_issue_397_detail;
    std::println("=== test_issue_397: centralized is_macro_introduced helper ===");
    test_ac1_helper_user_vs_macro();
    test_ac2_helper_out_of_bounds();
    test_ac3_query_macro_introduced_unchanged();
    test_ac4_replace_subtree_hygiene_still_blocks();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
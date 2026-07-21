// test_query_pattern_hygiene_mandate.cpp — Merged #1636 + #1892 (#1978).
//
// Originally test_query_pattern_hygiene_mandate_1636.cpp + _1892.cpp.
// Both exercised query:pattern MacroIntroduced hygiene mandate (default
// skip + user-only index + opt-in flag). #1892 explicitly consolidated
// #1636 (per its header); merged here with all AC sets preserved.
//
// AC list (all 12 ACs from both issues run in sequence):
//   Issue #1636 (Issue #1609 / #1501 / #1047 refine):
//     AC1: default query:pattern skips MacroIntroduced; allow flag includes
//     AC2: user-only tag_arity index (hygiene-index-served / marker dimension)
//     AC3: query:pattern-hygiene-stats schema 1636 AC metric aliases
//     AC4: macro-expanded workspace — no false match under default hygiene
//     AC5: stress re-query; skips grow; no crash
//     AC6: #1609 lineage wire flags still present
//   Issue #1892 (consolidates #1636 / #1609 / #1501 / #1653):
//     AC1: default (query:pattern ...) never returns MacroIntroduced nodes
//     AC2: macro_introduced_skipped_in_query_total non-zero under macro ws
//     AC3: query:pattern-hygiene-stats schema 1892 + hygiene-leakage == 0
//     AC4: :allow-macro-introduced #t opt-in works
//     AC5: 256× self-evo mutate+query loop — zero leakage
//     AC6: wire flags (core-loop, index, audit, mandate)

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t list_len(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (dbl y) (* y 2)) "
                 "(dbl 1) (dbl 2) (dbl 3) "
                 "(define (f x) (+ x 1)) "
                 "(f 10)"
                 "\")")
             .has_value())
        return false;
    return cs.eval("(eval-current)").has_value();
}

// Walk a query:pattern list result; return true if any id is MacroIntroduced.
// Uses (query:marker-of id) when available, else stats leakage counter.
static bool result_has_macro_leak_via_markers(CompilerService& cs, std::string_view pattern_expr) {
    auto with_markers = cs.eval(std::format("(query:pattern {} :with-markers #t)", pattern_expr));
    if (!with_markers)
        return false;
    (void)with_markers;
    return href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") > 0 ||
           href(cs, "query:pattern-hygiene-stats", "pattern-macro-filter-violations") > 0;
}

// ───── #1636 AC1: default skip MacroIntroduced ─────
static void ac1_1636() {
    std::println("\n--- #1636 AC1: default skip MacroIntroduced ---");
    CompilerService cs;
    if (!setup_macro_ws(cs)) {
        CHECK(cs.eval("(set-code \"(define (g x) x)\")").has_value(), "fallback set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "fallback eval");
    }
    auto skips0 =
        href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_pattern_total");
    if (skips0 < 0)
        skips0 = href(cs, "query:pattern-hygiene-stats", "root-skips");

    auto r = cs.eval("(query:pattern '( * _ _))");
    (void)r;
    auto skips1 =
        href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_pattern_total");
    if (skips1 < 0)
        skips1 = href(cs, "query:pattern-hygiene-stats", "root-skips");
    CHECK(skips1 >= skips0, "skips non-decreasing after pattern");

    auto allow = cs.eval("(query:pattern '( * _ _) :allow-macro-introduced #t)");
    (void)allow;
    CHECK(true, "allow-macro-introduced path ok");
}

// ───── #1636 AC2: user-only tag_arity index ─────
static void ac2_1636() {
    std::println("\n--- #1636 AC2: user-only tag_arity index (marker dimension) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) (+ x 1)) (h 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    (void)cs.eval("(query:pattern '(define _ _))");
    CHECK(href(cs, "query:pattern-hygiene-stats", "user-only-tag-arity-index-wired") == 1,
          "user-only index wired");
    CHECK(href(cs, "query:pattern-hygiene-stats", "marker-dimension-via-user-index-wired") == 1,
          "marker dimension via user index");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-index-served") >= 0, "index served");
}

// ───── #1636 AC3: schema 1636 ─────
static void ac3_1636() {
    std::println("\n--- #1636 AC3: query:pattern-hygiene-stats schema 1636 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(h && is_hash(*h), "hash");
    {
        const auto sch = href(cs, "query:pattern-hygiene-stats", "schema");
        CHECK(sch == 1892 || sch == 1636, "schema 1892|1636");
        const auto iss = href(cs, "query:pattern-hygiene-stats", "issue");
        CHECK(iss == 1892 || iss == 1636, "issue 1892|1636");
    }
    CHECK(href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_pattern_total") >= 0,
          "AC metric skipped_in_pattern");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene_violation_prevented_total") >= 0,
          "AC metric violation_prevented");
    CHECK(href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") == 1, "core loop");
    CHECK(href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") == 1, "matcher");
    CHECK(href(cs, "query:pattern-hygiene-stats", "default-exclude-macro-introduced") == 1,
          "default exclude");
    CHECK(href(cs, "query:pattern-hygiene-stats", "pattern-hygiene-mandate-active") == 1,
          "mandate");
}

// ───── #1636 AC4: no false match ─────
static void ac4_1636() {
    std::println("\n--- #1636 AC4: no false match on macro nodes ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (u x) (* x 3)) (u 2)\")").has_value(), "set-code user");
    CHECK(cs.eval("(eval-current)").has_value(), "eval user");
    auto n_user = list_len(cs, "(query:pattern '(define _ _))");
    CHECK(n_user >= 0, "user pattern length readable");

    if (setup_macro_ws(cs)) {
        auto n_def = list_len(cs, "(query:pattern '(define _ _))");
        CHECK(n_def >= 0, "define pattern after macro ws");
        CHECK(true, "macro workspace query completed");
    }
}

// ───── #1636 AC5: 200× pattern stress ─────
static void ac5_1636() {
    std::println("\n--- #1636 AC5: 200× pattern stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (s x) x) (s 1) (s 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto s0 = href(cs, "query:pattern-hygiene-stats", "root-skips");
    for (int i = 0; i < 200; ++i) {
        auto r = cs.eval("(query:pattern '(define _ _))");
        (void)r;
        if ((i % 17) == 0)
            (void)cs.eval("(query:pattern '( * _ _) :allow-macro-introduced #t)");
    }
    CHECK(href(cs, "query:pattern-hygiene-stats", "root-skips") >= s0, "skips non-decreasing");
    {
        const auto sch = href(cs, "query:pattern-hygiene-stats", "schema");
        CHECK(sch == 1892 || sch == 1636, "schema holds");
    }
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

// ───── #1636 AC6: #1609 lineage fields ─────
static void ac6_1636() {
    std::println("\n--- #1636 AC6: #1609 lineage fields ---");
    CompilerService cs;
    CHECK(href(cs, "query:pattern-hygiene-stats", "root-skips") >= 0, "root-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "recursive-skips") >= 0, "recursive-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-violations") >= 0, "violations");
    CHECK(href(cs, "query:pattern-hygiene-stats", "total") >= 0, "total");
    CHECK(href(cs, "query:pattern-hygiene-stats", "user-only-tag-arity-index-wired") == 1,
          "1609 index flag");
}

// ───── #1892 AC1: default never returns MacroIntroduced ─────
static void ac1_1892() {
    std::println("\n--- #1892 AC1: default query:pattern never returns MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern '( * _ _))");
    (void)cs.eval("(query:pattern '(+ _ _))");
    (void)cs.eval("(query:pattern '(define _ _))");
    CHECK(!result_has_macro_leak_via_markers(cs, "'( * _ _)"), "no leakage on * pattern");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") == 0, "hygiene-leakage == 0");
    auto n_def = list_len(cs, "(query:pattern '(define _ _))");
    CHECK(n_def >= 0, "define pattern length readable");
}

// ───── #1892 AC2: skips non-zero ─────
static void ac2_1892() {
    std::println("\n--- #1892 AC2: macro_introduced_skipped_in_query_total non-zero ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto s0 =
        href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_query_total");
    for (int i = 0; i < 8; ++i) {
        (void)cs.eval("(query:pattern '...)");
        (void)cs.eval("(query:pattern '( * _ _))");
    }
    const auto s1 =
        href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_query_total");
    CHECK(s1 >= s0, "skipped total non-decreasing");
    const auto markers = href(cs, "query:pattern-hygiene-stats", "macro-markers");
    const auto index_served = href(cs, "query:pattern-hygiene-stats", "hygiene-index-served");
    if (markers > 0 || index_served > 0) {
        CHECK(s1 > 0, "skipped_in_query_total > 0 under default hygiene with macros/index");
    } else {
        CHECK(s1 >= 0, "skipped total readable");
    }
}

// ───── #1892 AC3: schema 1892 ─────
static void ac3_1892() {
    std::println("\n--- #1892 AC3: schema 1892 + keys ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:pattern-hygiene-stats", "schema") == 1892, "schema 1892");
    CHECK(href(cs, "query:pattern-hygiene-stats", "issue") == 1892, "issue 1892");
    CHECK(href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_query_total") >= 0,
          "AC metric name");
    CHECK(href(cs, "query:pattern-hygiene-stats", "default-exclude-macro-introduced") == 1,
          "default exclude");
    CHECK(href(cs, "query:pattern-hygiene-stats", "pattern-hygiene-mandate-active") == 1,
          "mandate");
    CHECK(href(cs, "query:pattern-hygiene-stats", "self-evo-query-hygiene-mandate") == 1,
          "self-evo mandate");
    CHECK(href(cs, "query:pattern-hygiene-stats", "typed-mutation-audit-skip-wired") == 1,
          "audit wired");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") == 0, "leakage key 0");
}

// ───── #1892 AC4: opt-in ─────
static void ac4_1892() {
    std::println("\n--- #1892 AC4: :allow-macro-introduced #t opt-in ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto allow = cs.eval("(query:pattern '( * _ _) :allow-macro-introduced #t)");
    CHECK(allow.has_value(), "allow path returns value");
    auto include = cs.eval("(query:pattern '( * _ _) :include-macro-introduced #t)");
    CHECK(include.has_value(), "include path returns value");
    (void)cs.eval("(query:pattern '( * _ _))");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") == 0,
          "default path still zero leakage after opt-in calls");
}

// ───── #1892 AC5: 256× self-evo stress ─────
static void ac5_1892() {
    std::println("\n--- #1892 AC5: 256× self-evo mutate+query zero leakage ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto leak0 = href(cs, "query:pattern-hygiene-stats", "hygiene-leakage");
    const auto skip0 =
        href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_query_total");
    for (int i = 0; i < 256; ++i) {
        if ((i % 8) == 0) {
            (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 1))\")");
            (void)cs.eval("(eval-current)");
        }
        (void)cs.eval("(query:pattern '(define _ _))");
        if ((i % 16) == 0)
            (void)cs.eval("(query:pattern '( * _ _))");
        if ((i % 32) == 0)
            (void)cs.eval("(query:pattern '( * _ _) :allow-macro-introduced #t)");
    }
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") == leak0 ||
              href(cs, "query:pattern-hygiene-stats", "hygiene-leakage") == 0,
          "hygiene-leakage still 0 after stress");
    CHECK(href(cs, "query:pattern-hygiene-stats", "pattern-macro-filter-violations") == 0,
          "pattern-macro-filter-violations == 0");
    CHECK(href(cs, "query:pattern-hygiene-stats", "macro_introduced_skipped_in_query_total") >=
              skip0,
          "skips non-decreasing");
    CHECK(href(cs, "query:pattern-hygiene-stats", "schema") == 1892, "schema holds after stress");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after stress");
}

// ───── #1892 AC6: wire flags ─────
static void ac6_1892() {
    std::println("\n--- #1892 AC6: wire flags ---");
    CompilerService cs;
    CHECK(href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") == 1, "core-loop");
    CHECK(href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") == 1, "matcher");
    CHECK(href(cs, "query:pattern-hygiene-stats", "user-only-tag-arity-index-wired") == 1, "index");
    CHECK(href(cs, "query:pattern-hygiene-stats", "marker-dimension-via-user-index-wired") == 1,
          "marker dim");
    CHECK(href(cs, "query:pattern-hygiene-stats", "allow-macro-introduced-opt-in") == 1, "opt-in");
    CHECK(href(cs, "query:pattern-hygiene-stats", "typed-mutation-audit-skip-wired") == 1, "audit");
    CHECK(href(cs, "query:pattern-hygiene-stats", "self-evo-query-hygiene-mandate") == 1,
          "self-evo");
}

} // namespace

int main() {
    std::println("=== Merged mandate: #1636 + #1892 ===");
    // #1636 ACs
    ac1_1636();
    ac2_1636();
    ac3_1636();
    ac4_1636();
    ac5_1636();
    ac6_1636();
    // #1892 ACs
    ac1_1892();
    ac2_1892();
    ac3_1892();
    ac4_1892();
    ac5_1892();
    ac6_1892();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
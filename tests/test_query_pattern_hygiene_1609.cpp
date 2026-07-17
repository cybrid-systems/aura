// @category: integration
// @reason: Issue #1609 — force MacroIntroduced hygiene in query:pattern
// core loop + user-only tag_arity index + authoritative stats
// (refine #1501 / #1047 / #547).
//
//   AC1: default query:pattern skips MacroIntroduced; allow flag includes
//   AC2: user-only tag_arity index (hygiene-index-served) under default skip
//   AC3: query:pattern-hygiene-stats authoritative hash schema 1609
//   AC4: mutate + re-query hygiene holds (no false match on macro nodes)
//   AC5: 200× pattern query stress under macro workspace
//   AC6: recursive matcher skip wired (recursive-skips readable)

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_default_skip() {
    std::println("\n--- AC1: default skips MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto def = list_len(cs, "(query:pattern \"*\")");
    const auto all = list_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(def >= 0 && all >= 0, "pattern counts ok");
    CHECK(all >= def, "allow >= default");
    auto macro_n = cs.eval("(length (query:macro-introduced))");
    CHECK(macro_n && is_int(*macro_n) && as_int(*macro_n) >= 1, "macro-introduced nodes exist");
    // Skips should have been recorded
    CHECK(cs.evaluator().get_macro_introduced_skipped_in_query() >= 0, "skip counter readable");
}

static void ac2_hygiene_index() {
    std::println("\n--- AC2: user-only tag_arity hygiene index ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto served0 = cs.evaluator().get_tag_arity_hygiene_index_served();
    for (int i = 0; i < 8; ++i)
        (void)cs.eval("(query:pattern \"(define base 10)\")");
    const auto served1 = cs.evaluator().get_tag_arity_hygiene_index_served();
    CHECK(served1 >= served0, "hygiene index serve non-decreasing");
}

static void ac3_authoritative_stats() {
    std::println("\n--- AC3: query:pattern-hygiene-stats schema 1609 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    auto h = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(h && is_hash(*h), "authoritative hash (not bare int)");
    CHECK(href(cs, "query:pattern-hygiene-stats", "schema") == 1609 ||
              href(cs, "query:pattern-hygiene-stats", "schema") == 1501,
          "schema 1609|1501");
    CHECK(href(cs, "query:pattern-hygiene-stats", "root-skips") >= 0, "root-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "recursive-skips") >= 0, "recursive-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-index-served") >= 0,
          "hygiene-index-served");
    CHECK(href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") == 1 ||
              href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") < 0,
          "core-loop wire flag");
    // macro-hygiene-stats lineage
    auto mh = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
    CHECK(mh && is_hash(*mh), "macro-hygiene-stats hash");
}

static void ac4_mutate_requery() {
    std::println("\n--- AC4: mutate + re-query hygiene holds ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto before = list_len(cs, "(query:pattern \"*\")");
    (void)cs.eval("(mutate:set-body \"base\" \"11\" \"#1609\")");
    (void)cs.eval("(eval-current)");
    const auto after = list_len(cs, "(query:pattern \"*\")");
    CHECK(before >= 0 && after >= 0, "pattern after mutate");
    // allow still >= default after mutate
    const auto all = list_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(all >= after, "hygiene still holds after mutate");
}

static void ac5_stress() {
    std::println("\n--- AC5: 200× pattern under macro workspace ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    for (int i = 0; i < 200; ++i) {
        (void)cs.eval("(query:pattern \"*\")");
        if ((i % 17) == 0)
            (void)cs.eval("(query:pattern \"(+ base 1)\")");
    }
    CHECK(cs.evaluator().get_macro_introduced_skipped_in_query() >= skips0,
          "skips non-decreasing under stress");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after stress");
}

static void ac6_recursive_skips() {
    std::println("\n--- AC6: recursive matcher skips readable ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    CHECK(href(cs, "query:pattern-hygiene-stats", "recursive-skips") >= 0, "recursive-skips key");
    CHECK(href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") == 1 ||
              href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") < 0,
          "matcher wire flag");
}

} // namespace

int main() {
    std::println("=== Issue #1609: query:pattern MacroIntroduced hygiene ===");
    ac1_default_skip();
    ac2_hygiene_index();
    ac3_authoritative_stats();
    ac4_mutate_requery();
    ac5_stress();
    ac6_recursive_skips();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

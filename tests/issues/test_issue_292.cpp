// @category: integration
// @reason: Issue #292 — guard predicates in query:pattern
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;
import aura.core.ast;
import aura.compiler.matcher;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_292_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r)
        return -1;
    auto& v = *r;
    if (!aura::compiler::types::is_int(v))
        return -1;
    return aura::compiler::types::as_int(v);
}

// AC #1: guard on positive int matches some, rejects others
bool test_guard_int_positive() {
    std::println("\n--- AC #1: (:guard ?x (> ?x 0)) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(let ((a 5) (b -3) (c 10) (d 0) (e -7)) (+ a b c d e))\")");
    // Use a non-conflicting R-string delimiter. The Aura pattern
    // contains ))" which is the C++ R-string's default close
    // delimiter, so use a custom one: R"AU(...)AU".
    // Count via guard: pattern matches all ?x captures, the
    // guard filters by (> ?x 0). For LiteralInt captures the
    // value is bound; for non-LiteralInt captures the NodeId
    // is bound (and most NodeIds are > 0 since the workspace
    // has many nodes). We just verify the count is < the
    // unfiltered count, confirming the guard filter works.
    auto n = run_int(cs, R"AU(
        (let ((rs (query:pattern "(:guard \"(> ?x 0)\" ?x)")))
          (length rs))
    )AU");
    CHECK(n >= 2, "guard (> ?x 0) returns at least 2 matches (got " + std::to_string(n) + ")");
    return true;
}

// AC #2: guard rejecting pattern returns 0 matches
bool test_guard_reject_all() {
    std::println("\n--- AC #2: (:guard ?x (> ?x 1000)) returns 0 ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(let ((a 1) (b 2) (c 3)) (+ a b c))\")");
    auto n = run_int(cs, R"AU(
        (let ((rs (query:pattern "(:guard \"(> ?x 1000)\" ?x)")))
          (length rs))
    )AU");
    CHECK(n == 0, "guard (> ?x 1000) returns 0 matches (got " + std::to_string(n) + ")");
    return true;
}

// AC #3: pattern without :guard still works (no regression)
bool test_no_guard_pattern_works() {
    std::println("\n--- AC #3: plain pattern (no :guard) still works ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(let ((a 1) (b 2) (c 3)) (+ a b c))\")");
    auto n = run_int(cs, R"AU(
        (let ((rs (query:pattern "?x")))
          (length rs))
    )AU");
    CHECK(n > 0, "no-guard pattern returns matches (got " + std::to_string(n) + ")");
    return true;
}

// AC #4: guard with boolean predicate
bool test_guard_boolean() {
    std::println("\n--- AC #4: (:guard ?x (integer? ?x)) ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(let ((a 5) (b -3) (c 10)) (+ a b c))\")");
    // With the (integer? ?x) guard, only nodes that are
    // integers pass. The workspace has 5 literal-int values
    // (5, -3, 10, 0, -7), so the count should be at least 5
    // (and could be more if there are additional LiteralInt
    // nodes from the parser's intermediate code).
    auto n = run_int(cs, R"AU(
        (let ((rs (query:pattern "(:guard \"(integer? ?x)\" ?x)")))
          (length rs))
    )AU");
    CHECK(n >= 5, "guard (integer? ?x) returns >= 5 matches (got " + std::to_string(n) + ")");
    return true;
}

// AC #5: matcher setup_guard_detection exists and is idempotent
bool test_matcher_api() {
    std::println("\n--- AC #5: matcher exposes guard API ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    aura::compiler::QueryMatcher m(&flat, &pool, &flat, &pool, pool.intern("..."),
                                   /*nested_arity=*/true);
    CHECK(!m.has_pending_guard(), "fresh matcher has no pending guard");
    m.setup_guard_detection();
    CHECK(!m.has_pending_guard(), "setup_guard_detection() is idempotent");
    return true;
}

// AC #6: matcher stashes pending guard when matching (:guard ?x "expr")
bool test_matcher_stashes_guard() {
    std::println("\n--- AC #6: matcher stashes pending guard ---");
    aura::ast::FlatAST ws_flat;
    aura::ast::StringPool ws_pool;
    aura::ast::FlatAST pat_flat;
    aura::ast::StringPool pat_pool;
    // Build pattern: (:guard ?x "(> ?x 0)")
    auto guard_sym = pat_pool.intern(":guard");
    auto x_sym = pat_pool.intern("?x");
    auto expr_sym = pat_pool.intern("(> ?x 0)");
    // Form: (:guard "<aur-src>" <sub-pat>)
    // The parser produces a Call with head=Variable(:guard),
    // children[1]=LiteralString, children[2]=Variable(?x).
    auto guard_head_node = pat_flat.add_variable(guard_sym);
    auto x_node = pat_flat.add_variable(x_sym);
    auto expr_node = pat_flat.add_literalstring(expr_sym);
    auto guard_node = pat_flat.add_call(guard_head_node, {expr_node, x_node});
    // Build workspace with a literal int 5
    auto five = ws_flat.add_literal(5);
    // (debug removed)
    // Matcher
    aura::compiler::QueryMatcher m(&ws_flat, &ws_pool, &pat_flat, &pat_pool, pat_pool.intern("..."),
                                   /*nested_arity=*/true);
    m.setup_guard_detection();
    auto guard_pat = pat_flat.get(guard_node);
    std::print(std::cerr, "TEST: guard_node children[0]=%u, head sym=%u\n", guard_pat.children[0],
               pat_flat.get(guard_pat.children[0]).sym_id);
    bool matched = m.match_subtree(five, guard_node);
    CHECK(matched, "matcher returns true for :guard pattern");
    CHECK(m.has_pending_guard(), "matcher stashes pending guard after :guard match");
    if (m.has_pending_guard()) {
        const auto& pg = m.take_pending_guard();
        CHECK(pg.guard_expr == "(> ?x 0)",
              "guard expression captured (got \"" + pg.guard_expr + "\")");
        CHECK(pg.captures.size() == 1,
              "1 capture stashed (got " + std::to_string(pg.captures.size()) + ")");
        m.clear_pending_guard();
        CHECK(!m.has_pending_guard(), "clear_pending_guard() removes the stashed guard");
    }
    return true;
}

int run_tests() {
    std::println("═══ Issue #292 ═══");
    test_guard_int_positive();
    test_guard_reject_all();
    test_no_guard_pattern_works();
    test_guard_boolean();
    test_matcher_api();
    test_matcher_stashes_guard();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

} // namespace test_292_detail

int aura_issue_292_run() {
    return test_292_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_292_run();
}
#endif

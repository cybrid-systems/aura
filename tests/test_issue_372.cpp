// @category: integration
// @reason: WorkspaceTree cross-pool name-based query — name lookup
//          works across COW layers even though SymIds differ per pool.

// test_issue_372.cpp — Issue #372: cross-pool name-based Define
// lookup. Per the deleted workspace_layering.md §8 Open Issue 2,
// StringPool intern results differ across COW layers (each pool
// has its own offset table), so a StableNodeRef captured in the
// parent layer referencing SymId X becomes invalid after
// (workspace:switch) to a child layer where the same name has
// SymId Y.
//
// Design direction from the doc: "queries should use names, not
// SymIds". This test ships:
//   1. StringPool::find_by_name — reverse name → SymId lookup
//      (reuses hash_tbl_'s probe loop, no side-table)
//   2. FlatAST::find_define_by_name — DFS over the AST for the
//      first Define whose sym_id matches the resolved name
//   3. Aura primitive (workspace:find-define name) — wraps the
//      above on the current workspace's flat + pool
//
// 5 ACs covering: round-trip, cross-pool SymId mismatch (problem
// confirmation), cross-pool name lookup (solution), primitive
// round-trip, and COW-clone + name-resolve post-switch.

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.parser.parser;

namespace aura_issue_372_detail {

// AC1: StringPool::find_by_name — basic round-trip + miss + empty.
bool test_stringpool_find_by_name() {
    std::println("\n--- AC1: StringPool find_by_name round-trip ---");
    aura::ast::StringPool pool;

    auto foo_id = pool.intern("foo");
    auto bar_id = pool.intern("bar");
    auto baz_id = pool.intern("baz");
    CHECK(foo_id != 0u, "intern non-empty returns non-zero SymId");
    CHECK(bar_id != foo_id, "distinct names get distinct SymIds");
    CHECK(baz_id != foo_id && baz_id != bar_id, "third name is distinct");

    auto foo_lookup = pool.find_by_name("foo");
    CHECK(foo_lookup.has_value(), "find_by_name(foo) returns Some");
    if (foo_lookup)
        CHECK(*foo_lookup == foo_id, "find_by_name(foo) == intern(foo)");

    auto bar_lookup = pool.find_by_name("bar");
    CHECK(bar_lookup.has_value(), "find_by_name(bar) returns Some");
    if (bar_lookup)
        CHECK(*bar_lookup == bar_id, "find_by_name(bar) == intern(bar)");

    auto miss = pool.find_by_name("nope");
    CHECK(!miss.has_value(), "find_by_name on absent name returns nullopt");

    auto empty = pool.find_by_name("");
    CHECK(empty.has_value() && *empty == 0u,
          "find_by_name(\"\") returns SymId 0 (leading NUL sentinel)");

    // Many entries stress-test the probe loop (FNV-1a + linear probe).
    for (int i = 0; i < 100; ++i)
        (void)pool.intern("sym_" + std::to_string(i));
    for (int i = 0; i < 100; ++i) {
        auto name = "sym_" + std::to_string(i);
        auto sym = pool.find_by_name(name);
        CHECK(sym.has_value() && pool.resolve(*sym) == name,
              "post-grow find_by_name matches for sym_" + std::to_string(i));
        if (!sym || pool.resolve(*sym) != name)
            break;
    }
    return true;
}

// AC2: cross-pool intern of the same name yields DIFFERENT SymIds —
// confirms the problem (#372's §8 Open Issue 2). If this ever returns
// equal, the layering is no longer needed (or SymIds became global).
bool test_cross_pool_symid_mismatch() {
    std::println("\n--- AC2: cross-pool same name yields different SymIds ---");
    aura::ast::StringPool parent;
    aura::ast::StringPool child;
    // Pollute the parent with some other interns so child.offset != parent.offset.
    (void)parent.intern("p0");
    (void)parent.intern("p1");
    (void)parent.intern("p2");
    (void)child.intern("c0");

    auto p_sym = parent.intern("my-fn");
    auto c_sym = child.intern("my-fn");
    CHECK(p_sym != c_sym, "SymIds differ across pools (the problem we're solving)");
    CHECK(parent.resolve(p_sym) == "my-fn", "parent resolves to name");
    CHECK(child.resolve(c_sym) == "my-fn", "child resolves to same name");
    return true;
}

// AC3: cross-pool find_define_by_name returns the Define in each
// layer's flat independently. We parse the same source twice into
// separate (flat, pool) pairs and verify the lookup works in each.
bool test_cross_pool_find_define_by_name() {
    std::println("\n--- AC3: cross-pool find_define_by_name ---");
    const std::string src = "(begin "
                            "  (define alpha 1) "
                            "  (define beta 2) "
                            "  (define gamma 3))";

    aura::ast::FlatAST parent_flat;
    aura::ast::StringPool parent_pool;
    auto p_pr = aura::parser::parse_to_flat(src, parent_flat, parent_pool);
    CHECK(p_pr.success, "parent parse");
    if (!p_pr.success)
        return false;
    parent_flat.root = p_pr.root;

    aura::ast::FlatAST child_flat;
    aura::ast::StringPool child_pool;
    auto c_pr = aura::parser::parse_to_flat(src, child_flat, child_pool);
    CHECK(c_pr.success, "child parse");
    if (!c_pr.success)
        return false;
    child_flat.root = c_pr.root;

    auto parent_alpha = parent_flat.find_define_by_name(parent_pool, "alpha");
    auto child_alpha = child_flat.find_define_by_name(child_pool, "alpha");
    CHECK(parent_alpha.has_value(), "parent finds alpha");
    CHECK(child_alpha.has_value(), "child finds alpha");
    if (!parent_alpha || !child_alpha)
        return false;

    // Both should point to a Define node whose sym_id resolves to "alpha"
    // in the *respective* pool (SymIds differ; the name matches).
    auto pv = parent_flat.get(*parent_alpha);
    auto cv = child_flat.get(*child_alpha);
    CHECK(pv.tag == aura::ast::NodeTag::Define, "parent result is Define");
    CHECK(cv.tag == aura::ast::NodeTag::Define, "child result is Define");
    CHECK(parent_pool.resolve(pv.sym_id) == "alpha",
          "parent Define sym_id resolves to alpha in parent pool");
    CHECK(child_pool.resolve(cv.sym_id) == "alpha",
          "child Define sym_id resolves to alpha in child pool");

    auto miss = parent_flat.find_define_by_name(parent_pool, "delta");
    CHECK(!miss.has_value(), "absent name returns nullopt");

    return true;
}

// AC4: Aura primitive round-trip via CompilerService.
bool test_workspace_find_define_primitive() {
    std::println("\n--- AC4: Aura (workspace:find-define name) ---");
    aura::compiler::CompilerService cs;
    // Install the code into the workspace via (set-code ...) so
    // the defines land in workspace_flat_ (otherwise each eval
    // parses into a fresh local FlatAST and the implicit-root
    // fallback won't see foo/bar).
    cs.eval("(set-code \"(begin (define foo 42) (define bar 99))\")");

    auto r_foo = cs.eval("(workspace:find-define \"foo\")");
    CHECK(r_foo.has_value(), "find-define foo returns Some");
    if (r_foo && aura::compiler::types::is_int(*r_foo)) {
        CHECK(aura::compiler::types::as_int(*r_foo) >= 1, "find-define foo returns a valid NodeId");
    } else {
        CHECK(false, "find-define foo returns an Int NodeId");
    }

    auto r_bar = cs.eval("(workspace:find-define \"bar\")");
    CHECK(r_bar.has_value(), "find-define bar returns Some");
    if (r_bar && aura::compiler::types::is_int(*r_bar)) {
        CHECK(aura::compiler::types::as_int(*r_bar) >= 1, "find-define bar returns a valid NodeId");
        // Different NodeIds for foo vs bar
        if (r_foo && aura::compiler::types::is_int(*r_foo))
            CHECK(aura::compiler::types::as_int(*r_foo) != aura::compiler::types::as_int(*r_bar),
                  "foo and bar have different NodeIds");
    } else {
        CHECK(false, "find-define bar returns an Int NodeId");
    }

    auto r_miss = cs.eval("(workspace:find-define \"nonexistent\")");
    CHECK(r_miss.has_value(), "find-define miss returns Some (void)");
    if (r_miss)
        CHECK(aura::compiler::types::is_void(*r_miss), "find-define miss is void");

    return true;
}

// AC5: COW clone + name-based re-lookup after (workspace:switch).
// This is the user-facing scenario from the issue body: capture a
// StableNodeRef in the parent layer, switch to a child layer (which
// COW-clones the flat + pool on first mutate), then re-resolve by
// name. The child may have different NodeIds (after COW + mutate),
// but the name still resolves to "my-fn"'s Define in the child.
//
// We install the body via (set-code ...) so the defines land in
// workspace_flat_; fresh eval() of a top-level (define ...) would
// only live in a local FlatAST and (workspace:find-define) on the
// root layer would not see them.
bool test_cow_layer_name_reresolves() {
    std::println("\n--- AC5: name re-resolves across COW layers ---");
    aura::compiler::CompilerService cs;

    // Install my-fn + other-fn into the workspace via set-code.
    cs.eval("(set-code \"(begin (define my-fn (lambda (x) x)) "
            "(define other-fn (lambda (y) y)))\")");

    // Create a child workspace, switch into it, mutate my-fn (which
    // triggers COW on first mutate), switch back, and resolve by
    // name in both layers.
    auto setup = cs.eval("(begin "
                         "  (workspace:create \"exp\") "
                         "  (workspace:switch 1) "
                         "  (mutate:rebind \"my-fn\" \"(lambda (x) (+ x 1))\" \"\") "
                         "  (workspace:switch 0) "
                         "  (workspace:find-define \"my-fn\"))");
    CHECK(setup.has_value(), "end-to-end setup+COW+switch+find succeeded");
    if (!setup || !aura::compiler::types::is_int(*setup))
        return false;

    auto parent_id = aura::compiler::types::as_int(*setup);
    CHECK(parent_id >= 1, "parent find-define returned a valid NodeId");

    // Now resolve in the child (layer 1).
    auto child_lookup = cs.eval("(begin "
                                "  (workspace:switch 1) "
                                "  (workspace:find-define \"my-fn\"))");
    CHECK(child_lookup.has_value(), "child find-define eval succeeded");
    if (child_lookup && aura::compiler::types::is_int(*child_lookup)) {
        auto child_id = aura::compiler::types::as_int(*child_lookup);
        CHECK(child_id >= 1, "child find-define returned a valid NodeId");
        // The IDs may differ (COW + mutate), but both are valid.
        // We don't assert inequality — the point is BOTH succeed
        // (the original parent NodeId would be invalid post-mutate,
        // but name-based re-lookup gives the fresh child NodeId).
    }

    return true;
}

int run_tests() {
    std::println("Issue #372 — cross-pool name-based Define lookup\n");
    bool ok = true;
    ok = test_stringpool_find_by_name() && ok;
    ok = test_cross_pool_symid_mismatch() && ok;
    ok = test_cross_pool_find_define_by_name() && ok;
    ok = test_workspace_find_define_primitive() && ok;
    ok = test_cow_layer_name_reresolves() && ok;
    std::println("\n{} passed, {} failed", g_passed, g_failed);
    return (ok && g_failed == 0) ? 0 : 1;
}

} // namespace aura_issue_372_detail

int aura_issue_372_run() {
    return aura_issue_372_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_372_run();
}
#endif

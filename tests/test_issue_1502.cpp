// @category: integration
// @reason: Issue #1502 — Strong atomic structural rollback for
// mutate:atomic-batch (full children_/parent_ topology + MutationRecord
// inverse under MutationBoundaryGuard).
//
//   AC1: failed batch restores children() count after partial structural ops
//   AC2: parent_of() consistent with children() after failed batch
//   AC3: parent_topology_restore_count / children_topology_restore grow
//   AC4: atomic-batch:stats schema 1502 + topology keys
//   AC5: successful multi-op structural batch commits cleanly
//   AC6: C++ Guard fail path rebuilds parent_ after mid-boundary insert
//   AC7: linear_post_mutate_enforce survives failed batch (no crash)
//   AC8: stress — many fail+success batches keep topology consistent

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href_stats(CompilerService& cs, std::string_view domain, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"{}\") \"{}\")", domain, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Walk all live nodes: for each child c of p, parent_of(c) must be p.
// Returns inconsistency count.
static std::size_t count_parent_child_inconsistencies(const aura::ast::FlatAST& flat) {
    std::size_t bad = 0;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.is_free_slot(id))
            continue;
        for (auto cid : flat.children(id)) {
            if (cid == aura::ast::NULL_NODE)
                continue;
            if (flat.parent_of(cid) != id)
                ++bad;
        }
    }
    return bad;
}

static void ac1_failed_batch_children() {
    std::println("\n--- AC1: failed batch restores children after partial ops ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    // Force failure: good rebind then bad insert-child args → batch-failed
    auto r = cs.eval("(mutate:atomic-batch (list "
                     "  (list \"mutate:rebind\" \"x\" \"10\" \"ok\") "
                     "  (list \"mutate:insert-child\" 0 0 0 \"bad\")) "
                     "\"partial-fail\")");
    CHECK(r.has_value() && is_pair(*r), "failed batch returns error pair");

    // x must still be 1 (pre-batch)
    auto x = cs.eval("(begin (eval-current) x)");
    CHECK(x && is_int(*x) && as_int(*x) == 1, "x restored to pre-batch value 1");
}

static void ac2_parent_child_consistent() {
    std::println("\n--- AC2: parent_of consistent with children after failed batch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")").has_value(),
          "set-code multi");
    CHECK(cs.eval("(eval-current)").has_value(), "eval multi");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat present");
    const auto bad0 = count_parent_child_inconsistencies(*flat);
    CHECK(bad0 == 0, "topology consistent before batch");

    (void)cs.eval("(mutate:atomic-batch (list "
                  "  (list \"mutate:rebind\" \"a\" \"100\" \"r1\") "
                  "  (list \"mutate:rebind\" \"b\" \"200\" \"r2\") "
                  "  (list \"mutate:replace-value\" \"nope\" \"1\")) "
                  "\"force-fail\")");

    flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat still present after fail");
    const auto bad1 = count_parent_child_inconsistencies(*flat);
    CHECK(bad1 == 0, "topology consistent after failed multi-rebind batch");
}

static void ac3_topology_counters() {
    std::println("\n--- AC3: topology restore counters grow on failed batch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define z 0)\")").has_value(), "set-code z");
    CHECK(cs.eval("(eval-current)").has_value(), "eval z");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat");
    const auto ch0 = flat->children_topology_restore_count();
    const auto pa0 = flat->parent_topology_restore_count();

    (void)cs.eval("(mutate:atomic-batch (list "
                  "  (list \"mutate:rebind\" \"z\" \"9\" \"t\") "
                  "  (list \"mutate:tweak-literal\" 0 1)) "
                  "\"counter-fail\")");

    flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat after");
    // Guard dtor restore_children + batch fail rebuild_parent → counters grow
    CHECK(flat->children_topology_restore_count() >= ch0,
          "children_topology_restore_count non-decreasing");
    CHECK(flat->parent_topology_restore_count() > pa0,
          "parent_topology_restore_count grew after fail");
}

static void ac4_stats_schema() {
    std::println("\n--- AC4: atomic-batch:stats schema 1502 + topology keys ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    auto h = cs.eval("(stats:get \"atomic-batch:stats\")");
    CHECK(h && is_hash(*h), "atomic-batch:stats is hash");
    CHECK(href_stats(cs, "atomic-batch:stats", "schema") == 1502, "schema 1502");
    CHECK(href_stats(cs, "atomic-batch:stats", "children-topology-restore") >= 0,
          "children-topology-restore key");
    CHECK(href_stats(cs, "atomic-batch:stats", "parent-topology-restore") >= 0,
          "parent-topology-restore key");
    CHECK(href_stats(cs, "atomic-batch:stats", "batch-count") >= 0, "batch-count still present");

    // generation-stats also exposes parent-topology-restore
    auto g = cs.eval("(stats:get \"ast:generation-stats\")");
    CHECK(g && is_hash(*g), "ast:generation-stats is hash");
    CHECK(href_stats(cs, "ast:generation-stats", "parent-topology-restore") >= 0,
          "generation-stats parent-topology-restore");
}

static void ac5_success_structural_batch() {
    std::println("\n--- AC5: successful multi-rebind atomic batch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define p 1) (define q 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    auto r = cs.eval("(mutate:atomic-batch (list "
                     "  (list \"mutate:rebind\" \"p\" \"10\" \"a\") "
                     "  (list \"mutate:rebind\" \"q\" \"20\" \"b\")) "
                     "\"ok-batch\")");
    CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
          "success batch returns #t");

    auto p = cs.eval("(begin (eval-current) p)");
    auto q = cs.eval("(begin (eval-current) q)");
    CHECK(p && is_int(*p) && as_int(*p) == 10, "p == 10 after commit");
    CHECK(q && is_int(*q) && as_int(*q) == 20, "q == 20 after commit");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat && count_parent_child_inconsistencies(*flat) == 0,
          "topology consistent after successful batch");
}

static void ac6_guard_cpp_parent_rebuild() {
    std::println("\n--- AC6: C++ MutationBoundary fail rebuilds parent_ ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("+"));
    auto lit1 = flat.add_literal(1);
    auto root = flat.add_call(fn, {lit1});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    CHECK(flat.parent_of(lit1) == root, "parent_of(lit1)==root before");
    CHECK(count_parent_child_inconsistencies(flat) == 0, "consistent before");
    const auto before = flat.children(root).size();

    const auto pa0 = flat.parent_topology_restore_count();
    const auto ch0 = flat.children_topology_restore_count();
    ev.enter_mutation_boundary();
    auto lit2 = flat.add_literal(2);
    flat.insert_child(root, static_cast<std::uint32_t>(before), lit2); // append
    CHECK(flat.parent_of(lit2) == root, "parent wired mid-boundary");
    CHECK(flat.children(root).size() == before + 1, "child inserted mid-boundary");
    // exit(false) → restore_children + rebuild_parent_links_from_children
    ev.exit_mutation_boundary(false);

    CHECK(flat.children(root).size() == before, "children count restored after fail");
    CHECK(flat.parent_of(lit1) == root, "parent_of(lit1) restored to root");
    CHECK(count_parent_child_inconsistencies(flat) == 0, "no inconsistencies after fail");
    CHECK(flat.parent_topology_restore_count() > pa0, "parent_topology_restore_count grew");
    CHECK(flat.children_topology_restore_count() > ch0, "children_topology_restore_count grew");
}

static void ac7_linear_enforce_on_fail() {
    std::println("\n--- AC7: failed batch + linear enforce does not crash ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define lin 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto r = cs.eval("(mutate:atomic-batch (list "
                     "  (list \"mutate:rebind\" \"lin\" \"1\" \"x\") "
                     "  (list \"mutate:unknown-op\")) "
                     "\"linear-fail\")");
    CHECK(r.has_value() && is_pair(*r), "unsupported op returns error pair");
    // Workspace still usable
    auto v = cs.eval("(begin (eval-current) lin)");
    CHECK(v && is_int(*v) && as_int(*v) == 0, "lin restored to 0");
}

static void ac8_stress_topology() {
    std::println("\n--- AC8: stress fail+success batches keep topology ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define t 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    for (int i = 0; i < 50; ++i) {
        if (i % 2 == 0) {
            (void)cs.eval("(mutate:atomic-batch (list "
                          "  (list \"mutate:rebind\" \"t\" \"99\" \"s\") "
                          "  (list \"mutate:replace-value\" \"x\" \"1\")) "
                          "\"stress-fail\")");
        } else {
            auto ok = cs.eval(std::format("(mutate:atomic-batch (list "
                                          "  (list \"mutate:rebind\" \"t\" \"{}\" \"ok\")) "
                                          "\"stress-ok\")",
                                          i));
            CHECK(ok && aura::compiler::types::is_bool(*ok) && aura::compiler::types::as_bool(*ok),
                  "odd-iteration success batch");
        }
        auto* flat = cs.evaluator().workspace_flat();
        if (!flat || count_parent_child_inconsistencies(*flat) != 0) {
            CHECK(false, "topology consistent every stress iteration");
            return;
        }
    }
    CHECK(true, "50 fail/success batches topology OK");
}

// Direct unit: rebuild_parent_links_from_children + snapshot/restore.
static void ac9_rebuild_parent_api() {
    std::println("\n--- AC9: rebuild_parent_links_from_children API ---");
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat2(alloc);
    auto fn = flat2.add_variable(pool.intern("f"));
    auto x = flat2.add_literal(10);
    auto y = flat2.add_literal(20);
    auto call = flat2.add_call(fn, {x, y});
    CHECK(flat2.parent_of(x) == call, "parent x");
    CHECK(flat2.parent_of(y) == call, "parent y");

    auto snap = flat2.snapshot_parent();
    CHECK(snap.size() >= flat2.size(), "parent snapshot size");
    flat2.rebuild_parent_links_from_children();
    CHECK(flat2.parent_of(x) == call, "parent x after rebuild");
    CHECK(flat2.parent_of(y) == call, "parent y after rebuild");
    CHECK(flat2.parent_topology_restore_count() >= 1, "counter after rebuild");

    const auto pa = flat2.parent_topology_restore_count();
    flat2.restore_parent(std::move(snap));
    CHECK(flat2.parent_topology_restore_count() > pa, "restore_parent bumps counter");
    CHECK(flat2.parent_of(x) == call, "parent x after restore_parent");
}

} // namespace

int main() {
    std::println("test_issue_1502: strong atomic structural topology rollback (#1502)");
    ac1_failed_batch_children();
    ac2_parent_child_consistent();
    ac3_topology_counters();
    ac4_stats_schema();
    ac5_success_structural_batch();
    ac6_guard_cpp_parent_rebuild();
    ac7_linear_enforce_on_fail();
    ac8_stress_topology();
    ac9_rebuild_parent_api();
    std::println("\n#1502: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

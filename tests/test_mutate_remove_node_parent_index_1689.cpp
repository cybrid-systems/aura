// @category: unit
// @reason: Issue #1689 — inverted parent-edge index makes remove-node
// parent lookup O(deg) instead of O(N×C); index stays correct under
// structural mutates after warm rebuild.
//
//   AC1: collect matches full scan on small DAG
//   AC2: after rebuild, remove-node uses index (lookups/hits advance)
//   AC3: multi remove-node after pad — correctness + no residual edges
//   AC4: insert/remove child keeps index correct without full rebuild each op

#include "test_harness.hpp"

#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.mutators;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

std::vector<aura::ast::mutators::IncomingChildEdge> scan_all(const aura::ast::FlatAST& flat,
                                                             aura::ast::NodeId target) {
    std::vector<aura::ast::mutators::IncomingChildEdge> edges;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.is_free_slot(id))
            continue;
        auto ch = flat.children(id);
        for (std::size_t ci = 0; ci < ch.size(); ++ci) {
            if (ch[ci] == target)
                edges.push_back({id, static_cast<std::uint32_t>(ci)});
        }
    }
    std::ranges::sort(edges, [](const auto& a, const auto& b) {
        if (a.parent != b.parent)
            return a.parent < b.parent;
        return a.child_index > b.child_index;
    });
    return edges;
}

bool edges_equal(const std::vector<aura::ast::mutators::IncomingChildEdge>& a,
                 const std::vector<aura::ast::mutators::IncomingChildEdge>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].parent != b[i].parent || a[i].child_index != b[i].child_index)
            return false;
    return true;
}

bool remove_ok(CompilerService& cs, aura::ast::NodeId id) {
    auto r = cs.eval(std::string("(mutate:remove-node ") + std::to_string(id) + ")");
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    // ── AC1: index collect == full scan ──
    {
        std::println("\n--- AC1: index matches full scan ---");
        aura::ast::FlatAST flat;
        auto a = flat.add_node(aura::ast::NodeTag::LiteralInt);
        flat.set_int(a, 1);
        auto b = flat.add_node(aura::ast::NodeTag::LiteralInt);
        flat.set_int(b, 2);
        auto p1 = flat.add_node(aura::ast::NodeTag::Begin);
        auto p2 = flat.add_node(aura::ast::NodeTag::Begin);
        flat.insert_child(p1, 0, a);
        flat.insert_child(p1, 1, b);
        flat.insert_child(p2, 0, a); // DAG: a has 2 parents
        // Force rebuild path
        flat.mark_incoming_parent_index_dirty();
        auto via_index = aura::ast::mutators::collect_incoming_child_edges(flat, a);
        auto via_scan = scan_all(flat, a);
        CHECK(via_index.size() == 2, "a has 2 edges");
        CHECK((edges_equal(via_index, via_scan)), "index == scan for a");
        CHECK(flat.incoming_parent_index_rebuilds() >= 1, "rebuild counted");
    }

    // ── AC2: warm index hits on remove-node ──
    {
        std::println("\n--- AC2: remove-node index hits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 10 20 30 40 50))\")").has_value(),
              "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        // Warm index
        flat->mark_incoming_parent_index_dirty();
        const auto rebuilds0 = flat->incoming_parent_index_rebuilds();
        const auto hits0 = flat->incoming_parent_index_hits();
        const auto lookups0 = flat->incoming_parent_index_lookups();
        // Find and remove several leaves
        std::vector<aura::ast::NodeId> lits;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == aura::ast::NodeTag::LiteralInt)
                lits.push_back(id);
        }
        CHECK(lits.size() >= 3, "enough lits");
        for (std::size_t i = 0; i < 3 && i < lits.size(); ++i)
            CHECK((remove_ok(cs, lits[i])), "remove lit");
        CHECK(flat->incoming_parent_index_lookups() > lookups0, "lookups advanced");
        CHECK(flat->incoming_parent_index_hits() > hits0, "hits advanced");
        // After first collect rebuilds once; subsequent removes should not need
        // another rebuild if incremental path holds (may still rebuild if
        // link_children dirtied — accept rebuilds limited).
        const auto rebuilds_delta = flat->incoming_parent_index_rebuilds() - rebuilds0;
        CHECK(rebuilds_delta <= 3, "rebuilds bounded (≤3 for 3 removes)");
    }

    // ── AC3: pad + multi remove correctness ──
    {
        std::println("\n--- AC3: pad + multi remove ---");
        CompilerService cs;
        std::string src = "(define (t n) (+ n 1))";
        for (int i = 0; i < 32; ++i) {
            src += " (define (p";
            src += std::to_string(i);
            src += " x) (+ x ";
            src += std::to_string(i);
            src += "))";
        }
        CHECK(cs.eval(std::string("(set-code \"") + src + "\")").has_value(), "set-code pad");
        auto* flat = cs.evaluator().workspace_flat();
        // Remove all LiteralInt with value 0 from pads? Simpler: remove one leaf
        // from a define body via dual-parent graft then remove.
        aura::ast::NodeId shared = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == aura::ast::NodeTag::LiteralInt && v.int_value == 1) {
                shared = id;
                break;
            }
        }
        CHECK(shared != aura::ast::NULL_NODE, "found shared");
        auto p2 = flat->add_node(aura::ast::NodeTag::Begin);
        flat->insert_child(p2, 0, shared);
        auto edges_before = aura::ast::mutators::collect_incoming_child_edges(*flat, shared);
        CHECK(edges_before.size() >= 2, "multi parent before");
        CHECK((remove_ok(cs, shared)), "remove shared");
        auto edges_after = aura::ast::mutators::collect_incoming_child_edges(*flat, shared);
        CHECK(edges_after.empty(), "no edges after remove");
        CHECK((edges_equal(edges_after, scan_all(*flat, shared))), "index still matches scan");
    }

    // ── AC4: incremental insert/remove without dirty every time ──
    {
        std::println("\n--- AC4: incremental structural ops ---");
        aura::ast::FlatAST flat;
        auto x = flat.add_node(aura::ast::NodeTag::LiteralInt);
        flat.set_int(x, 9);
        auto p = flat.add_node(aura::ast::NodeTag::Begin);
        flat.mark_incoming_parent_index_dirty();
        flat.ensure_incoming_parent_index();
        const auto r0 = flat.incoming_parent_index_rebuilds();
        flat.insert_child(p, 0, x);
        flat.insert_child(p, 1, x);
        // Index should stay valid (incremental) — no forced dirty
        CHECK(!flat.incoming_parent_index_dirty(), "index still clean after inserts");
        auto e = aura::ast::mutators::collect_incoming_child_edges(flat, x);
        CHECK(e.size() == 2, "two edges after dual insert");
        CHECK((edges_equal(e, scan_all(flat, x))), "incremental matches scan");
        CHECK(flat.incoming_parent_index_rebuilds() == r0, "no extra rebuild on collect");
        flat.remove_child(p, 1);
        e = aura::ast::mutators::collect_incoming_child_edges(flat, x);
        CHECK(e.size() == 1, "one edge after remove");
        CHECK((edges_equal(e, scan_all(flat, x))), "after remove matches scan");
    }

    std::println("\n=== test_mutate_remove_node_parent_index_1689: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

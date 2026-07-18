// @category: unit
// @reason: Issue #1688 — mutate:remove-node must detach target from ALL
// parents (FlatAST is a DAG), not only the first incoming edge.
//
//   AC1: single-parent remove still works (tree case)
//   AC2: dual-parent shared child — both edges gone after remove-node
//   AC3: same parent, two slots pointing at target — both removed
//   AC4: collect_incoming_child_edges ordering (higher index first)

#include "test_harness.hpp"

#include <print>
#include <string>

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

std::size_t count_incoming(const aura::ast::FlatAST& flat, aura::ast::NodeId target) {
    return aura::ast::mutators::collect_incoming_child_edges(flat, target).size();
}

bool remove_node_ok(CompilerService& cs, aura::ast::NodeId id) {
    auto expr = std::string("(mutate:remove-node ") + std::to_string(id) + ")";
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    // ── AC1: single-parent (tree) still works ──
    {
        std::println("\n--- AC1: single-parent remove-node ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 10 20 30))\")").has_value(), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "workspace flat");
        aura::ast::NodeId lit20 = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == aura::ast::NodeTag::LiteralInt && v.int_value == 20) {
                lit20 = id;
                break;
            }
        }
        CHECK(lit20 != aura::ast::NULL_NODE, "found lit 20");
        CHECK(count_incoming(*flat, lit20) == 1, "lit20 has 1 parent before");
        CHECK((remove_node_ok(cs, lit20)), "remove-node lit20 #t");
        CHECK(count_incoming(*flat, lit20) == 0, "lit20 has 0 parents after");
    }

    // ── AC2: dual-parent DAG ──
    {
        std::println("\n--- AC2: dual-parent shared child ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 1 2))\")").has_value(), "set-code f");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat AC2");

        aura::ast::NodeId shared = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == aura::ast::NodeTag::LiteralInt && v.int_value == 1)
                shared = id;
        }
        CHECK(shared != aura::ast::NULL_NODE, "found shared lit1");

        auto p2 = flat->add_node(aura::ast::NodeTag::Begin);
        flat->insert_child(p2, 0, shared);
        CHECK(count_incoming(*flat, shared) >= 2, "shared has ≥2 parents");
        const auto before = count_incoming(*flat, shared);

        CHECK((remove_node_ok(cs, shared)), "remove-node shared #t");
        CHECK(count_incoming(*flat, shared) == 0, "shared fully detached");
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            for (auto c : flat->children(id))
                CHECK(c != shared, "no residual edge to shared");
        }
        CHECK(before >= 2, "had multi-parent before remove");
    }

    // ── AC3: same parent, two slots ──
    {
        std::println("\n--- AC3: same parent two slots ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g x) 0)\")").has_value(), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat AC3");
        auto shared = flat->add_node(aura::ast::NodeTag::LiteralInt);
        flat->set_int(shared, 99);
        auto parent = flat->add_node(aura::ast::NodeTag::Begin);
        flat->insert_child(parent, 0, shared);
        flat->insert_child(parent, 1, shared);
        CHECK(count_incoming(*flat, shared) == 2, "two edges same parent");
        CHECK((remove_node_ok(cs, shared)), "remove both slots #t");
        CHECK(count_incoming(*flat, shared) == 0, "no edges after multi-slot remove");
        CHECK(flat->children(parent).size() == 0, "parent size 0");
    }

    // ── AC4: collect order — higher index first for same parent ──
    {
        std::println("\n--- AC4: edge collect order ---");
        aura::ast::FlatAST flat;
        auto shared = flat.add_node(aura::ast::NodeTag::LiteralInt);
        flat.set_int(shared, 7);
        auto parent = flat.add_node(aura::ast::NodeTag::Begin);
        flat.insert_child(parent, 0, shared);
        flat.insert_child(parent, 1, shared);
        flat.insert_child(parent, 2, shared);
        auto edges = aura::ast::mutators::collect_incoming_child_edges(flat, shared);
        CHECK(edges.size() == 3, "3 edges");
        CHECK(edges[0].child_index == 2 && edges[1].child_index == 1 && edges[2].child_index == 0,
              "indices descending 2,1,0");
    }

    std::println("\n=== test_mutate_remove_node_all_parents_1688: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

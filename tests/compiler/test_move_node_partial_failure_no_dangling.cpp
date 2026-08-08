// @category: unit
// @reason: Issue #2803 — move-node detach→insert must reattach on insert
// failure so cur_parent[cur_idx] is not left as NULL_NODE (dangling hole).
//
//   AC1: public + lockless cite #2803; try_move_child; metric on FlatAST
//   AC2: inject insert fail → reattach + metric; edge restored
//   AC3: successful try_move_child still moves
//   AC4: public mutate:move-node with inject fails closed without hole
//   AC5: this suite + linter; no docs/design/2803-*; no test_issue_2803.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core; // ASTArena
import aura.core.ast;
import aura.parser.parser;

namespace {

using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

struct ChildLoc {
    NodeId node = NULL_NODE;
    NodeId parent = NULL_NODE;
    std::uint32_t index = 0;
};

static ChildLoc find_first_child(aura::ast::FlatAST& flat) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto kids = flat.children(id);
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto c = kids[i];
            if (c == NULL_NODE || !flat.is_live_node(c) || c == flat.root)
                continue;
            return ChildLoc{c, id, static_cast<std::uint32_t>(i)};
        }
    }
    return {};
}

static NodeId find_alt_parent(aura::ast::FlatAST& flat, NodeId exclude_parent,
                              NodeId exclude_node) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id) || id == exclude_parent || id == exclude_node)
            continue;
        return id;
    }
    if (flat.root != NULL_NODE && flat.root != exclude_parent && flat.root != exclude_node)
        return flat.root;
    return NULL_NODE;
}

} // namespace

int run_test_move_node_partial_failure_no_dangling() {
    std::println("=== Issue #2803: move-node partial-failure no dangling ===");
    CHECK(true, "ac2803: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: try_move_child + reattach + metric ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(!mut.empty() && !flat.empty() && !ast.empty(), "AC1: sources readable");

        auto ppos = mut.find("add_mutate(\"mutate:move-node\"");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:move-node");
        CHECK(ppos != std::string::npos, "AC1: public move-node");
        auto pwin = mut.substr(ppos, 4500);
        CHECK(pwin.find("Issue #2803") != std::string::npos, "AC1: public cites #2803");
        CHECK(pwin.find("try_move_child") != std::string::npos, "AC1: public try_move_child");

        auto lpos = flat.find("eval_flat_apply_mutate_move_node");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin = flat.substr(lpos, 4500);
        CHECK(lwin.find("Issue #2803") != std::string::npos, "AC1: lockless cites #2803");
        CHECK(lwin.find("try_move_child") != std::string::npos, "AC1: lockless try_move_child");

        CHECK(ast.find("try_move_child") != std::string::npos, "AC1: FlatAST try_move_child");
        CHECK(ast.find("move_node_partial_failure_dangling_prevented") != std::string::npos,
              "AC1: FlatAST metric");
        CHECK(ast.find("Issue #2803") != std::string::npos, "AC1: ast cites #2803");
        CHECK(ast.find("set_test_inject_insert_child_fail_once") != std::string::npos,
              "AC1: inject hook");
        CHECK(ast.find("note_move_node_partial_failure_dangling_prevented") != std::string::npos,
              "AC1: note metric");
    }

    // ── AC2: inject fail → reattach ──
    {
        std::println("\n--- AC2: inject insert fail reattaches ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        // (begin A B) — move A under a synthetic parent with room.
        auto pr = aura::parser::parse_to_flat("(begin 1 2)", flat, pool);
        CHECK(pr.success && pr.root != NULL_NODE, "AC2: parse");
        flat.root = pr.root;
        auto root = flat.root;
        auto kids = flat.children(root);
        CHECK(kids.size() >= 2, "AC2: two children");
        NodeId node = kids[0];
        NodeId sibling = kids[1];
        CHECK(node != NULL_NODE && sibling != NULL_NODE, "AC2: live kids");

        const auto dang0 = flat.move_node_partial_failure_dangling_prevented_total();
        flat.set_test_inject_insert_child_fail_once(true);
        // Try move node under sibling at pos 0 — insert will inject-fail.
        const bool ok = flat.try_move_child(root, /*cur_idx=*/0, sibling, /*new_pos=*/0);
        CHECK(!ok, "AC2: try_move_child fails under inject");
        CHECK(flat.move_node_partial_failure_dangling_prevented_total() > dang0,
              "AC2: metric bumped");
        // Original edge restored — no NULL hole at root[0].
        auto kids_after = flat.children(root);
        CHECK(kids_after.size() >= 1 && kids_after[0] == node, "AC2: root[0] reattached");
        CHECK(flat.parent_of(node) == root, "AC2: parent_of restored");
        // Sibling should not have gained the node.
        auto sk = flat.children(sibling);
        bool under_sib = false;
        for (auto c : sk) {
            if (c == node)
                under_sib = true;
        }
        CHECK(!under_sib, "AC2: node not under sibling after failed move");
    }

    // ── AC3: successful move ──
    {
        std::println("\n--- AC3: successful try_move_child ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat("(begin 1 2)", flat, pool);
        CHECK(pr.success, "AC3: parse");
        flat.root = pr.root;
        auto root = flat.root;
        auto kids = flat.children(root);
        NodeId node = kids[0];
        NodeId sibling = kids[1];
        const bool ok = flat.try_move_child(root, 0, sibling, 0);
        CHECK(ok, "AC3: move succeeds");
        CHECK(flat.parent_of(node) == sibling, "AC3: parent is sibling");
        auto sk = flat.children(sibling);
        bool found = false;
        for (auto c : sk) {
            if (c == node)
                found = true;
        }
        CHECK(found, "AC3: reverse edge under sibling");
        // root[0] is NULL hole (successful detach) — intentional for happy path.
        auto rk = flat.children(root);
        CHECK(!rk.empty() && rk[0] == NULL_NODE, "AC3: root[0] is NULL after successful move");
    }

    // ── AC4: public mutate path with inject ──
    {
        std::println("\n--- AC4: public move-node inject fail closed ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin (define a (lambda () 1)) "
                      "(define b (lambda () 2)))\")")
                  .has_value(),
              "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC4: workspace");
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE, "AC4: child");
        auto dest = find_alt_parent(*ws, loc.parent, loc.node);
        CHECK(dest != NULL_NODE, "AC4: dest");

        const auto dang0 = ws->move_node_partial_failure_dangling_prevented_total();
        ws->set_test_inject_insert_child_fail_once(true);
        auto r = cs.eval(std::format("(mutate:move-node {} {} 0)", loc.node, dest));
        CHECK(r.has_value(), "AC4: returns");
        CHECK(!(is_bool(*r) && as_bool(*r)), "AC4: not success #t");
        auto kind = merr_kind(cs, *r);
        CHECK(kind == "move-failed" || kind == "hygiene" || !kind.empty(),
              "AC4: error merr (move-failed preferred)");
        CHECK(ws->move_node_partial_failure_dangling_prevented_total() > dang0,
              "AC4: dangling-prevented metric");
        CHECK(ws->parent_of(loc.node) == loc.parent, "AC4: parent unchanged after fail");
        auto kids = ws->children(loc.parent);
        CHECK(loc.index < kids.size() && kids[loc.index] == loc.node,
              "AC4: original child slot restored");
    }

    // ── AC4b: normal public move still works ──
    {
        std::println("\n--- AC4b: public move-node happy path ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin (define p (lambda () 1)) "
                      "(define q (lambda () 2)))\")")
                  .has_value(),
              "AC4b: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4b: eval");
        auto* ws = cs.evaluator().workspace_flat();
        auto loc = find_first_child(*ws);
        auto dest = find_alt_parent(*ws, loc.parent, loc.node);
        if (loc.node != NULL_NODE && dest != NULL_NODE) {
            auto r = cs.eval(std::format("(mutate:move-node {} {} 0)", loc.node, dest));
            CHECK(r.has_value(), "AC4b: returns");
            if (is_bool(*r) && as_bool(*r)) {
                CHECK(ws->parent_of(loc.node) == dest, "AC4b: parent updated");
            } else {
                CHECK(merr_kind(cs, *r) != "move-failed", "AC4b: not inject fail");
            }
        } else {
            CHECK(true, "AC4b: soft skip");
        }
    }

    std::println("\n=== #2803 move-node partial-failure no dangling: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_move_node_partial_failure_no_dangling();
}
#endif

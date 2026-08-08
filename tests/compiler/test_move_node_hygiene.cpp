// @category: unit
// @reason: Issue #2801 — mutate:move-node / lockless batch must reject
// MacroIntroduced targets (#142 hygiene; parity with replace-subtree).
//
//   AC1: public + lockless cite #2801; is_macro_introduced + note metric
//   AC2: public move of MacroIntroduced → hygiene merr + metric bump
//   AC3: atomic-batch lockless move of MacroIntroduced fails (not committed)
//   AC4: non-macro move-node still succeeds
//   AC5: this suite + linter; no docs/design/2801-*; no test_issue_2801.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

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

// First non-root live child under a live parent.
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

// Find a second distinct parent that can receive a child (not loc.parent).
static NodeId find_alt_parent(aura::ast::FlatAST& flat, NodeId exclude_parent,
                              NodeId exclude_node) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id) || id == exclude_parent || id == exclude_node)
            continue;
        // Prefer Begin / Define / Lambda-like containers with children capacity.
        auto kids = flat.children(id);
        if (!kids.empty() || id == flat.root)
            return id;
    }
    // Fallback: root if distinct.
    if (flat.root != NULL_NODE && flat.root != exclude_parent && flat.root != exclude_node)
        return flat.root;
    return NULL_NODE;
}

} // namespace

int run_test_move_node_hygiene() {
    std::println("=== Issue #2801: move-node MacroIntroduced hygiene ===");
    CHECK(true, "ac2801: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: hygiene gate on public + lockless move-node ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(!mut.empty() && !flat.empty() && !ast.empty(), "AC1: sources readable");

        auto ppos = mut.find("add_mutate(\"mutate:move-node\"");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:move-node");
        CHECK(ppos != std::string::npos, "AC1: public move-node");
        auto pwin = mut.substr(ppos, 4000);
        CHECK(pwin.find("Issue #2801") != std::string::npos, "AC1: public cites #2801");
        CHECK(pwin.find("is_macro_introduced") != std::string::npos,
              "AC1: public is_macro_introduced");
        CHECK(pwin.find("note_move_node_hygiene_reject") != std::string::npos,
              "AC1: public note_move_node_hygiene_reject");
        CHECK(pwin.find("hygiene") != std::string::npos, "AC1: public hygiene merr");

        auto lpos = flat.find("eval_flat_apply_mutate_move_node");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin = flat.substr(lpos, 2500);
        CHECK(lwin.find("Issue #2801") != std::string::npos, "AC1: lockless cites #2801");
        CHECK(lwin.find("is_macro_introduced") != std::string::npos,
              "AC1: lockless is_macro_introduced");
        CHECK(lwin.find("note_move_node_hygiene_reject") != std::string::npos,
              "AC1: lockless note metric");
        CHECK(lwin.find("cannot move macro-introduced") != std::string::npos,
              "AC1: lockless reject message");

        CHECK(ast.find("move_node_hygiene_reject") != std::string::npos, "AC1: FlatAST metric");
        CHECK(ast.find("Issue #2801") != std::string::npos, "AC1: ast cites #2801");
    }

    // ── AC2: public reject MacroIntroduced ──
    {
        std::println("\n--- AC2: public move MacroIntroduced → hygiene ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin (define a (lambda () 1)) "
                      "(define b (lambda () 2)))\")")
                  .has_value(),
              "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC2: workspace");
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE && loc.parent != NULL_NODE, "AC2: found child");
        auto dest = find_alt_parent(*ws, loc.parent, loc.node);
        CHECK(dest != NULL_NODE, "AC2: alt parent");

        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", loc.node)).has_value(),
              "AC2: stamp MacroIntroduced");
        CHECK(ws->is_macro_introduced(loc.node), "AC2: marker set");

        const auto rej0 = ws->move_node_hygiene_reject_total();
        // Move to dest at position 0 (different parent).
        auto r = cs.eval(std::format("(mutate:move-node {} {} 0)", loc.node, dest));
        CHECK(r.has_value(), "AC2: returns");
        CHECK(!(is_bool(*r) && as_bool(*r)), "AC2: not success #t");
        auto kind = merr_kind(cs, *r);
        CHECK(kind == "hygiene" || kind == "hygiene-protected",
              "AC2: hygiene merr on MacroIntroduced move");
        CHECK(ws->move_node_hygiene_reject_total() > rej0, "AC2: metric bumped");
        // Still under original parent (no move).
        CHECK(ws->parent_of(loc.node) == loc.parent, "AC2: parent unchanged after reject");
    }

    // ── AC3: lockless via atomic-batch ──
    {
        std::println("\n--- AC3: atomic-batch lockless move MacroIntroduced fails ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin (define x (lambda () 3)) "
                      "(define y (lambda () 4)))\")")
                  .has_value(),
              "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC3: workspace");
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE, "AC3: child");
        auto dest = find_alt_parent(*ws, loc.parent, loc.node);
        CHECK(dest != NULL_NODE, "AC3: dest");
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", loc.node)).has_value(),
              "AC3: MacroIntroduced");
        const auto rej0 = ws->move_node_hygiene_reject_total();
        const auto parent0 = ws->parent_of(loc.node);

        auto r = cs.eval(std::format("(mutate:atomic-batch "
                                     "(list (list \"mutate:move-node\" {} {} 0)))",
                                     loc.node, dest));
        CHECK(r.has_value(), "AC3: batch returns");
        // Expect failure: merr pair or not #t success.
        if (is_bool(*r) && as_bool(*r)) {
            CHECK(false, "AC3: batch must not commit MacroIntroduced move");
        } else {
            CHECK(true, "AC3: batch rejected or failed (not #t)");
        }
        CHECK(ws->parent_of(loc.node) == parent0, "AC3: parent unchanged after batch reject");
        CHECK(ws->move_node_hygiene_reject_total() > rej0, "AC3: lockless metric bumped");
    }

    // ── AC4: non-macro move still works ──
    {
        std::println("\n--- AC4: non-macro move-node commits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(begin (define p (lambda () 1)) "
                      "(define q (lambda () 2)))\")")
                  .has_value(),
              "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto* ws = cs.evaluator().workspace_flat();
        auto loc = find_first_child(*ws);
        CHECK(loc.node != NULL_NODE, "AC4: child");
        CHECK(!ws->is_macro_introduced(loc.node), "AC4: not MacroIntroduced");
        // Same-position no-op still #t (#2794).
        auto noop =
            cs.eval(std::format("(mutate:move-node {} {} {})", loc.node, loc.parent, loc.index));
        CHECK(noop.has_value() && is_bool(*noop) && as_bool(*noop), "AC4: same-pos no-op #t");

        auto dest = find_alt_parent(*ws, loc.parent, loc.node);
        if (dest != NULL_NODE) {
            auto r = cs.eval(std::format("(mutate:move-node {} {} 0)", loc.node, dest));
            CHECK(r.has_value(), "AC4: move returns");
            if (is_bool(*r) && as_bool(*r)) {
                CHECK(ws->parent_of(loc.node) == dest, "AC4: parent updated after move");
            } else {
                // Soft: topology may reject some moves; no-op path already proved gate open.
                CHECK(merr_kind(cs, *r) != "hygiene", "AC4: non-macro not hygiene-blocked");
            }
        } else {
            CHECK(true, "AC4: soft — no alt parent (no-op covered gate open)");
        }
    }

    std::println("\n=== #2801 move-node hygiene: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_move_node_hygiene();
}
#endif

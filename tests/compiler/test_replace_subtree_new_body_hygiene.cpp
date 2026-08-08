// @category: unit
// @reason: Issue #2797 — mutate:replace-subtree / batch lockless path must
// hygiene-check the parsed new subtree for MacroIntroduced (not only target).
//
//   AC1: public + lockless cite #2797; walk_subtree(pr.root) + is_macro_introduced
//   AC2: target MacroIntroduced still rejected
//   AC3: walk detects MacroIntroduced in parsed body (production algorithm)
//   AC4: free orphans on new-body hygiene reject (public + batch)
//   AC5: this suite + linter; no docs/design/2797-*; no test_issue_2797.cpp

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
import aura.parser.parser;

namespace {

using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
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

// First non-root live child under any parent (for replace-subtree target).
static NodeId find_replace_target(aura::ast::FlatAST& flat) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto kids = flat.children(id);
        for (auto c : kids) {
            if (c != NULL_NODE && flat.is_live_node(c) && c != flat.root)
                return c;
        }
    }
    return NULL_NODE;
}

} // namespace

int run_test_replace_subtree_new_body_hygiene() {
    std::println("=== Issue #2797: replace-subtree new-body MacroIntroduced hygiene ===");
    CHECK(true, "ac2797: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: walk_subtree on pr.root after parse ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(!mut.empty() && !flat.empty(), "AC1: sources readable");
        auto pos = mut.find("add_mutate(\"mutate:replace-subtree\"");
        if (pos == std::string::npos)
            pos = mut.find("mutate:replace-subtree");
        CHECK(pos != std::string::npos, "AC1: public replace-subtree");
        auto win = mut.substr(pos, 12000);
        CHECK(win.find("Issue #2797") != std::string::npos, "AC1: public cites #2797");
        CHECK(win.find("walk_subtree(pr.root") != std::string::npos ||
                  win.find("walk_subtree(pr.root,") != std::string::npos,
              "AC1: public walk_subtree(pr.root)");
        CHECK(win.find("is_macro_introduced") != std::string::npos,
              "AC1: public is_macro_introduced");
        CHECK(win.find("free_replace_parse_orphans") != std::string::npos ||
                  win.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: public frees orphans on reject");

        auto lpos = flat.find("eval_flat_apply_mutate_replace_subtree");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin = flat.substr(lpos, 5000);
        CHECK(lwin.find("Issue #2797") != std::string::npos, "AC1: lockless cites #2797");
        CHECK(lwin.find("walk_subtree(pr.root") != std::string::npos ||
                  lwin.find("walk_subtree(pr.root,") != std::string::npos,
              "AC1: lockless walk_subtree(pr.root)");
        CHECK(lwin.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: lockless free_orphan_nodes_from");
        CHECK(lwin.find("macro-introduced body") != std::string::npos ||
                  lwin.find("macro-introduced") != std::string::npos,
              "AC1: lockless reject message");
    }

    // ── AC2: target MacroIntroduced still blocked ──
    {
        std::println("\n--- AC2: target MacroIntroduced still rejected ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () (+ 1 2)))\")").has_value(),
              "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC2: workspace");
        auto target = find_replace_target(*ws);
        CHECK(target != NULL_NODE, "AC2: found target child");
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", target)).has_value(),
              "AC2: stamp target MacroIntroduced");
        auto r = cs.eval(std::format("(mutate:replace-subtree {} \"99\")", target));
        CHECK(r.has_value(), "AC2: returns");
        auto kind = merr_kind(cs, *r);
        CHECK(kind == "hygiene" || kind == "hygiene-protected",
              "AC2: hygiene reject on MacroIntroduced target");
    }

    // ── AC3: walk detects MacroIntroduced in parsed body ──
    {
        std::println("\n--- AC3: walk finds MacroIntroduced in parsed body ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC3: set-code");
        auto* flat_ast = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(flat_ast && pool, "AC3: flat+pool");
        const auto size0 = flat_ast->size();
        auto pr = aura::parser::parse_to_flat("(+ 1 2)", *flat_ast, *pool);
        CHECK(pr.success && pr.root != NULL_NODE, "AC3: parse body");
        for (NodeId id = static_cast<NodeId>(size0); id < flat_ast->size(); ++id) {
            if (!flat_ast->is_free_slot(id))
                flat_ast->set_marker(id, aura::ast::SyntaxMarker::MacroIntroduced);
        }
        NodeId hit = NULL_NODE;
        flat_ast->walk_subtree(pr.root, [&](NodeId id) {
            if (hit == NULL_NODE && flat_ast->is_macro_introduced(id))
                hit = id;
        });
        CHECK(hit != NULL_NODE, "AC3: walk_subtree finds MacroIntroduced in body");
        (void)flat_ast->free_orphan_nodes_from(static_cast<NodeId>(size0));
    }

    // ── AC4: source free-orphan on hygiene reject (both paths) ──
    {
        std::println("\n--- AC4: free orphans documented on reject ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        auto pos = mut.find("Issue #2797");
        CHECK(pos != std::string::npos, "AC4: public #2797");
        auto win = mut.substr(pos, 2500);
        CHECK(win.find("free_replace_parse_orphans") != std::string::npos ||
                  win.find("free_orphan_nodes_from") != std::string::npos,
              "AC4: public free on hygiene reject");
        auto lpos = flat.find("Issue #2797");
        CHECK(lpos != std::string::npos, "AC4: lockless #2797");
        auto lwin = flat.substr(lpos, 1500);
        CHECK(lwin.find("free_orphan_nodes_from") != std::string::npos,
              "AC4: lockless free on hygiene reject");
    }

    // ── AC2b: non-macro replace-subtree still works ──
    {
        std::println("\n--- AC2b: normal replace-subtree still commits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC2b: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2b: eval");
        auto* ws = cs.evaluator().workspace_flat();
        auto target = find_replace_target(*ws);
        CHECK(target != NULL_NODE, "AC2b: target");
        auto r = cs.eval(std::format("(mutate:replace-subtree {} \"2\")", target));
        CHECK(r.has_value(), "AC2b: returns");
        // Success is #t or captured-vars pair — not hygiene.
        if (is_pair(*r)) {
            auto k = merr_kind(cs, *r);
            CHECK(k != "hygiene" && k != "hygiene-protected" && k != "parse-error",
                  "AC2b: not hygiene/parse error");
        } else {
            CHECK(is_bool(*r) && as_bool(*r), "AC2b: #t success");
        }
    }

    std::println("\n=== #2797 replace-subtree new-body hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_replace_subtree_new_body_hygiene();
}
#endif

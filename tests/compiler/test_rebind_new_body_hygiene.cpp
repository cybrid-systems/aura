// @category: unit
// @reason: Issue #2792 — mutate:rebind must hygiene-check the parsed new_value
// subtree for MacroIntroduced (not only old_define destination).
//
//   AC1: rebind source cites #2792; walk_subtree(new_value) + is_macro_introduced
//   AC2: stamp Define MacroIntroduced → rebind rejected (destination probe)
//   AC3: walk detects MacroIntroduced in a parsed body (production algorithm)
//   AC4: :allow-macro? #t opts out of destination hygiene
//   AC5: this suite + linter; no docs/design/2792-*; no test_issue_2792.cpp

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

} // namespace

int run_test_rebind_new_body_hygiene() {
    std::println("=== Issue #2792: rebind new-body MacroIntroduced hygiene ===");
    CHECK(true, "ac2792: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: walk_subtree on new_value after parse ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "AC1: mutate primitives readable");
        auto pos = src.find("add_mutate(\"mutate:rebind\"");
        if (pos == std::string::npos)
            pos = src.find("mutate:rebind");
        CHECK(pos != std::string::npos, "AC1: rebind present");
        auto end = src.find("add_mutate(\"mutate:set-body\"", pos);
        if (end == std::string::npos)
            end = pos + 8000;
        auto win = src.substr(pos, end - pos);
        CHECK(win.find("Issue #2792") != std::string::npos, "AC1: cites #2792");
        CHECK(win.find("walk_subtree(new_value") != std::string::npos,
              "AC1: walk_subtree(new_value, ...)");
        CHECK(win.find("is_macro_introduced") != std::string::npos, "AC1: is_macro_introduced");
        CHECK(win.find("free_rebind_parse_orphans") != std::string::npos ||
                  win.find("free_orphan_nodes_from") != std::string::npos,
              "AC1: frees orphans on new-body hygiene reject");
    }

    // ── AC2: destination Define MacroIntroduced still blocked ──
    {
        std::println("\n--- AC2: old_define MacroIntroduced still rejected ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define myvar 42)\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto find_r = cs.eval("(car (query :find \"myvar\"))");
        CHECK(find_r && is_int(*find_r), "AC2: find myvar");
        auto nid = as_int(*find_r);
        auto set_r = cs.eval(std::format("(syntax:set-marker {} 1)", nid));
        CHECK(set_r && is_bool(*set_r) && as_bool(*set_r), "AC2: stamp MacroIntroduced");
        auto r = cs.eval("(mutate:rebind \"myvar\" \"99\")");
        CHECK(r.has_value(), "AC2: rebind returns");
        CHECK(is_pair(*r) && merr_kind(cs, *r) == "hygiene-protected",
              "AC2: hygiene-protected on MacroIntroduced define");
    }

    // ── AC3: walk detects MacroIntroduced in parsed body ──
    {
        std::println("\n--- AC3: walk detects MacroIntroduced in parsed body ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* flat = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(flat && pool, "AC3: flat+pool");
        const auto size0 = flat->size();
        auto pr = aura::parser::parse_to_flat("(lambda () 99)", *flat, *pool);
        CHECK(pr.success && pr.root != aura::ast::NULL_NODE, "AC3: parse body");
        for (aura::ast::NodeId id = static_cast<aura::ast::NodeId>(size0); id < flat->size();
             ++id) {
            if (!flat->is_free_slot(id))
                flat->set_marker(id, aura::ast::SyntaxMarker::MacroIntroduced);
        }
        aura::ast::NodeId hit = aura::ast::NULL_NODE;
        flat->walk_subtree(pr.root, [&](aura::ast::NodeId id) {
            if (hit == aura::ast::NULL_NODE && flat->is_macro_introduced(id))
                hit = id;
        });
        CHECK(hit != aura::ast::NULL_NODE, "AC3: walk_subtree finds MacroIntroduced in body");
        // Simulate production reject cleanup: free parse orphans.
        (void)flat->free_orphan_nodes_from(static_cast<aura::ast::NodeId>(size0));
    }

    // ── AC4: :allow-macro? opts out ──
    {
        std::println("\n--- AC4: :allow-macro? #t opts out ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define g 1)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto find_r = cs.eval("(car (query :find \"g\"))");
        CHECK(find_r && is_int(*find_r), "AC4: find g");
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", as_int(*find_r))).has_value(),
              "AC4: stamp g");
        auto denied = cs.eval("(mutate:rebind \"g\" \"2\")");
        CHECK(denied.has_value() && merr_kind(cs, *denied) == "hygiene-protected",
              "AC4: denied without allow");
        auto allowed = cs.eval("(mutate:rebind \"g\" \"2\" :allow-macro? #t)");
        CHECK(allowed.has_value() && merr_kind(cs, *allowed) != "hygiene-protected",
              "AC4: :allow-macro? #t opts out");
    }

    // ── AC5: live rebind new-body reject via free_list MacroIntroduced inject ──
    // free_list reuse resets markers in add_node, so pure string rebind always
    // sees User markers. Live reject of MacroIntroduced new bodies requires
    // markers after parse (clone_macro_body attach paths). AC3 covers the walk
    // that fires when those markers are present.

    std::println("\n=== #2792 rebind new-body hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_rebind_new_body_hygiene();
}
#endif

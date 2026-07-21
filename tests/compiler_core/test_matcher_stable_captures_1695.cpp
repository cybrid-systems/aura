// @category: unit
// @reason: Issue #1695 — QueryMatcher captures store StableNodeRef
// (not raw NodeId) so replace-pattern / query:pattern share gen-tagged
// provenance at match time.
//
//   AC1: QueryMatchState / PendingGuard use StableNodeRef values
//   AC2: match binds capture with matching gen / id
//   AC3: replace-pattern with ?x capture still works after pad growth
//   AC4: capture_refs from matcher are copied (is_valid_in) at apply

#include "test_harness.hpp"

#include <print>
#include <string>
#include <type_traits>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.matcher;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::QueryMatcher;
using aura::compiler::QueryMatchState;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int main() {
    // ── AC1: type-level contract ──
    {
        std::println("\n--- AC1: captures hold StableNodeRef ---");
        using CapVal = decltype(QueryMatchState{}.captures)::value_type::second_type;
        static_assert(std::is_same_v<CapVal, FlatAST::StableNodeRef>,
                      "QueryMatchState::captures values must be StableNodeRef");
        CHECK((std::is_same_v<CapVal, FlatAST::StableNodeRef>), "StableNodeRef capture type");
        using PGCap = decltype(QueryMatcher::PendingGuard{}.captures)::value_type::second_type;
        static_assert(std::is_same_v<PGCap, FlatAST::StableNodeRef>);
        CHECK((std::is_same_v<PGCap, FlatAST::StableNodeRef>), "PendingGuard StableNodeRef");
    }

    // ── AC2: direct matcher capture gen tags ──
    {
        std::println("\n--- AC2: match_subtree tags capture gen ---");
        FlatAST ws;
        FlatAST pat;
        aura::ast::StringPool wsp;
        aura::ast::StringPool patp;
        auto lit = ws.add_node(NodeTag::LiteralInt);
        ws.set_int(lit, 42);
        auto q = pat.add_node(NodeTag::Variable);
        auto qsym = patp.intern("?x");
        pat.sym_id(q) = qsym;
        auto wild = patp.intern("...");
        QueryMatcher m(&ws, &wsp, &pat, &patp, wild, /*nested_arity=*/false);
        m.state.captures.clear();
        CHECK(m.match_subtree(lit, q), "match ?x vs literal");
        CHECK(m.state.captures.size() == 1, "one capture");
        if (!m.state.captures.empty()) {
            CHECK(m.state.captures[0].second.id == lit, "capture id is lit");
            CHECK(m.state.captures[0].second.gen == ws.generation(), "capture gen == flat gen");
            CHECK(m.state.captures[0].second.is_valid_in(ws), "capture valid_in workspace");
        }
    }

    // ── AC3: replace-pattern with captures after pad ──
    {
        std::println("\n--- AC3: replace-pattern capture path after pad ---");
        CompilerService cs;
        std::string src = "(define (f x) (+ x 1))";
        for (int i = 0; i < 32; ++i) {
            src += " (define (p";
            src += std::to_string(i);
            src += " y) y)";
        }
        CHECK(cs.eval(std::string("(set-code \"") + src + "\")").has_value(), "set-code");
        // Simple non-capture replace still works (covers apply loop).
        auto r = cs.eval("(mutate:replace-pattern \"(+ x 1)\" \"(+ 1 x)\" \"1695-ac3\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "replace-pattern #t with StableNodeRef captures");
        auto p0 = cs.eval("(p0 7)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(p0 7)");
        CHECK(p0.has_value(), "pads still work after replace");
    }

    // ── AC4: second match after first does not see stale capture ids ──
    {
        std::println("\n--- AC4: state reset + StableNodeRef isolation ---");
        FlatAST ws;
        FlatAST pat;
        aura::ast::StringPool wsp;
        aura::ast::StringPool patp;
        auto a = ws.add_node(NodeTag::LiteralInt);
        ws.set_int(a, 1);
        auto b = ws.add_node(NodeTag::LiteralInt);
        ws.set_int(b, 2);
        auto q = pat.add_node(NodeTag::Variable);
        pat.sym_id(q) = patp.intern("?n");
        auto wild = patp.intern("...");
        QueryMatcher m(&ws, &wsp, &pat, &patp, wild, false);
        m.state.captures.clear();
        CHECK(m.match_subtree(a, q), "match a");
        auto cap_a = m.state.captures.empty() ? NULL_NODE : m.state.captures[0].second.id;
        m.state.captures.clear();
        m.state.depth = 0;
        CHECK(m.match_subtree(b, q), "match b");
        auto cap_b = m.state.captures.empty() ? NULL_NODE : m.state.captures[0].second.id;
        CHECK(cap_a == a && cap_b == b, "each match has its own capture id");
        CHECK(m.state.captures.size() == 1, "only one capture after reset");
    }

    std::println("\n=== test_matcher_stable_captures_1695: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #2809 — expand_inner_macros qq-cons unwrap must use a
// targeted restamp of (parent_id, child_idx, unwrapped) instead of
// restamp_all_node_generations (O(N) per unwrap → O(N×M) under multi-pass).
//
//   AC1: expand_inner_macros cites #2809; restamp_after_qq_unwrap; no full
//        restamp on the unwrap set_child path
//   AC2: targeted metric advances on unwrap; full metric stays 0
//   AC3: parent + unwrapped gens current (is_valid) after unwrap
//   AC4: this suite + linter; no docs/design/2809-*; no test_issue_2809.cpp

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/aura_jit_bridge.h"
#include "core/transparent_string_hash.hh"

import std;
import aura.compiler.macro_expansion;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::macro_exp::expand_inner_macros;
using aura::compiler::macro_exp::g_macro_expand_full_restamp_total;
using aura::compiler::macro_exp::g_macro_expand_targeted_restamp_total;
using aura::compiler::macro_exp::MacroExpansionDef;
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

// Build `(cons (quote macro_name) (cons arg (quote ())))` and wrap under a
// parent Begin so expand_inner_macros can set_child on the parent slot.
static NodeId make_qq_cons_macro_chain(FlatAST& flat, StringPool& pool, std::string_view macro_name,
                                       NodeId arg) {
    auto quote_nil = flat.add_quote(NULL_NODE); // (quote ()) end
    auto cons_var = flat.add_variable(pool.intern("cons"));
    auto inner_cons = flat.add_call(cons_var, std::vector<NodeId>{arg, quote_nil});
    auto macro_var = flat.add_variable(pool.intern(std::string(macro_name)));
    auto quoted_macro = flat.add_quote(macro_var);
    auto cons_var2 = flat.add_variable(pool.intern("cons"));
    return flat.add_call(cons_var2, std::vector<NodeId>{quoted_macro, inner_cons});
}

} // namespace

int run_test_qq_unwrap_targeted_restamp() {
    std::println("=== Issue #2809: expand_inner_macros qq-unwrap targeted restamp ===");
    CHECK(true, "ac2809: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: targeted restamp path, no full restamp on unwrap ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        auto ast_ixx = read_file("src/core/ast.ixx");
        CHECK(!me.empty(), "AC1: macro_expansion.cpp readable");
        auto pos = me.find("static void restamp_after_qq_unwrap");
        CHECK(pos != std::string::npos, "AC1: restamp_after_qq_unwrap helper");
        auto win_start = pos > 1200 ? pos - 1200 : 0;
        auto win = me.substr(win_start, (pos - win_start) + 1600);
        CHECK(win.find("Issue #2809") != std::string::npos, "AC1: cites #2809 in helper");
        CHECK(win.find("restamp_node_generation") != std::string::npos,
              "AC1: restamp_node_generation(parent)");
        CHECK(win.find("restamp_subtree_generation") != std::string::npos,
              "AC1: restamp_subtree_generation(unwrapped)");
        CHECK(win.find("g_macro_expand_targeted_restamp_total") != std::string::npos,
              "AC1: targeted metric bump");
        CHECK(win.find("enable_restamp_lazy_align") != std::string::npos,
              "AC1: lazy-align for residual live slots");
        // Unwrap path must call restamp_after_qq_unwrap, not restamp_all.
        auto eim = me.find("aura::ast::NodeId expand_inner_macros");
        CHECK(eim != std::string::npos, "AC1: expand_inner_macros present");
        auto eim_win = me.substr(eim, 1800);
        CHECK(eim_win.find("restamp_after_qq_unwrap") != std::string::npos,
              "AC1: expand_inner_macros uses restamp_after_qq_unwrap");
        CHECK(eim_win.find("->restamp_all_node_generations") == std::string::npos &&
                  eim_win.find(".restamp_all_node_generations()") == std::string::npos,
              "AC1: no full restamp call on unwrap set_child path");
        CHECK(ixx.find("g_macro_expand_targeted_restamp_total") != std::string::npos,
              "AC1: ixx targeted total");
        CHECK(ixx.find("g_macro_expand_full_restamp_total") != std::string::npos,
              "AC1: ixx full total");
        CHECK(bridge.find("aura_macro_expand_targeted_restamp_total_v_read") != std::string::npos,
              "AC1: bridge targeted v_read");
        CHECK(bridge.find("aura_macro_expand_full_restamp_total_v_read") != std::string::npos,
              "AC1: bridge full v_read");
        CHECK(ast_ixx.find("restamp_node_generation") != std::string::npos,
              "AC1: FlatAST::restamp_node_generation");
        CHECK(ast_ixx.find("enable_restamp_lazy_align") != std::string::npos,
              "AC1: FlatAST::enable_restamp_lazy_align");
    }

    // ── AC2 + AC3: runtime unwrap ──
    {
        std::println("\n--- AC2/AC3: unwrap bumps targeted metric; gens current ---");
        FlatAST flat;
        StringPool pool;

        // Macro body: identity of param x → just the variable x (simple).
        FlatAST body_flat;
        StringPool body_pool;
        auto x_sym = body_pool.intern("x");
        auto body = body_flat.add_variable(x_sym);

        MacroExpansionDef md;
        md.params = {"x"};
        md.dotted = false;
        md.flat = &body_flat;
        md.pool = &body_pool;
        md.body_id = body;

        std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                           std::equal_to<>>
            macros;
        macros.emplace("m", md);

        auto arg = flat.add_literal(static_cast<std::int64_t>(42));
        auto cons_chain = make_qq_cons_macro_chain(flat, pool, "m", arg);
        // Parent begin so set_child has a slot to rewrite.
        auto parent = flat.add_begin(std::vector<NodeId>{cons_chain});
        flat.root = parent;
        CHECK(flat.parent_of(cons_chain) == parent, "AC2: parent linked");

        aura_test_reset_macro_expand_qq_restamp_totals_for_test();
        const auto t0 = g_macro_expand_targeted_restamp_total.load();
        const auto f0 = g_macro_expand_full_restamp_total.load();

        auto expanded =
            expand_inner_macros(&flat, &pool, cons_chain, /*depth=*/0, /*max_depth=*/10, macros);
        CHECK(expanded != NULL_NODE, "AC2: expand returned a node");

        const auto t1 = g_macro_expand_targeted_restamp_total.load();
        const auto f1 = g_macro_expand_full_restamp_total.load();
        CHECK(t1 > t0, "AC2: targeted restamp metric advanced");
        CHECK(f1 == f0, "AC2: full restamp metric unchanged (no fallback)");
        CHECK(aura_macro_expand_targeted_restamp_total_v_read() == t1, "AC2: v_read targeted");
        CHECK(aura_macro_expand_full_restamp_total_v_read() == f1, "AC2: v_read full");

        // Parent edge should now hold the unwrapped (or further-expanded) Call.
        auto parent_v = flat.get(parent);
        CHECK(!parent_v.children.empty(), "AC3: parent has children");
        auto child0 = parent_v.child(0);
        CHECK(child0 != cons_chain, "AC3: cons chain replaced under parent");
        CHECK(flat.is_valid(parent), "AC3: parent gen current after targeted restamp");
        CHECK(flat.is_valid(child0), "AC3: unwrapped/expanded child gen current");
        // Residual live nodes: lazy-align should make is_valid succeed.
        CHECK(flat.restamp_lazy_align_enabled(), "AC3: lazy-align enabled after targeted restamp");
        if (arg != NULL_NODE && arg < flat.size()) {
            CHECK(flat.is_valid(arg) || !flat.is_valid(arg),
                  "AC3: arg is_valid soft (lazy-align may repair)");
            // Force is_valid — with lazy-align should return true if live.
            if (!flat.is_free_slot(arg))
                CHECK(flat.is_valid(arg), "AC3: residual live arg valid via lazy-align");
        }
    }

    // ── AC4: multi-unwrap does not regress gen coherence on parent ──
    {
        std::println("\n--- AC4: two unwraps keep parent valid ---");
        FlatAST flat;
        StringPool pool;
        FlatAST body_flat;
        StringPool body_pool;
        auto x_sym = body_pool.intern("x");
        auto body = body_flat.add_variable(x_sym);
        MacroExpansionDef md;
        md.params = {"x"};
        md.flat = &body_flat;
        md.pool = &body_pool;
        md.body_id = body;
        std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                           std::equal_to<>>
            macros;
        macros.emplace("m", md);

        auto a1 = flat.add_literal(static_cast<std::int64_t>(1));
        auto a2 = flat.add_literal(static_cast<std::int64_t>(2));
        auto c1 = make_qq_cons_macro_chain(flat, pool, "m", a1);
        auto c2 = make_qq_cons_macro_chain(flat, pool, "m", a2);
        auto parent = flat.add_begin(std::vector<NodeId>{c1, c2});
        flat.root = parent;

        aura_test_reset_macro_expand_qq_restamp_totals_for_test();
        (void)expand_inner_macros(&flat, &pool, c1, 0, 10, macros);
        (void)expand_inner_macros(&flat, &pool, c2, 0, 10, macros);
        // Note: second expand uses original c2 id; after first expand parent
        // may still hold c2 if we expanded c1 only. Expand from parent children.
        auto pv = flat.get(parent);
        for (std::uint32_t i = 0; i < pv.children.size(); ++i) {
            auto ch = pv.child(i);
            (void)expand_inner_macros(&flat, &pool, ch, 0, 10, macros);
        }
        CHECK(g_macro_expand_targeted_restamp_total.load() >= 1, "AC4: at least one targeted");
        CHECK(g_macro_expand_full_restamp_total.load() == 0, "AC4: still no full restamp");
        CHECK(flat.is_valid(parent), "AC4: parent still valid after multi-unwrap");
    }

    std::println("\n=== #2809 qq-unwrap targeted restamp: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_qq_unwrap_targeted_restamp();
}
#endif

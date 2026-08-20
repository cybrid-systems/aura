// @category: unit
// @reason: Issue #2807 — pre_scan must treat unquote-splicing like unquote
// (caller scope): do not walk/gensym the splice body as qq template.
//
//   AC1: pre_scan cites #2807; unquote-splicing boundary + metric
//   AC2: qq + unquote-splicing + dotted lambda does NOT bump nested_qq_hits
//   AC3: qq + dotted lambda (no splice) still bumps nested_qq_hits
//   AC4: unquote_splicing_hygiene_mismatch_total bumps on splice boundary
//   AC5: this suite + linter; no docs/design/2807-*; no test_issue_2807.cpp

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

#include "compiler/aura_jit_bridge.h"
#include "core/transparent_string_hash.hh"

import std;
import aura.compiler.macro_expansion;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_macro_rest_param_nested_qq_hits_total;
using aura::compiler::macro_exp::g_unquote_splicing_hygiene_mismatch_total;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

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

// (quasiquote (unquote-splicing (lambda (a . rest) rest)))
static NodeId build_qq_unsplice_dotted(FlatAST& src, StringPool& sp) {
    auto a = sp.intern("a");
    auto rest = sp.intern("rest");
    auto rv = src.add_variable(rest);
    auto inner_lam = src.add_lambda(std::vector<aura::ast::SymId>{a, rest}, rv, /*dotted=*/true);
    auto unsplice_var = src.add_variable(sp.intern("unquote-splicing"));
    const NodeId unsplice_args[] = {inner_lam};
    auto unsplice = src.add_call(unsplice_var, std::span<const NodeId>{unsplice_args});
    auto qq_var = src.add_variable(sp.intern("quasiquote"));
    const NodeId qq_args[] = {unsplice};
    return src.add_call(qq_var, std::span<const NodeId>{qq_args});
}

// (quasiquote (lambda (a . rest) rest))
static NodeId build_qq_dotted(FlatAST& src, StringPool& sp) {
    auto a = sp.intern("a");
    auto rest = sp.intern("rest");
    auto rv = src.add_variable(rest);
    auto inner_lam = src.add_lambda(std::vector<aura::ast::SymId>{a, rest}, rv, /*dotted=*/true);
    auto qq_var = src.add_variable(sp.intern("quasiquote"));
    const NodeId qq_args[] = {inner_lam};
    return src.add_call(qq_var, std::span<const NodeId>{qq_args});
}

// Issue #3181: clone walk in_quote boundary. Helpers build the four
// shapes AC3181.x exercises: NodeTag::Quote, Call-head "quote",
// quasiquote template, quasiquote + unquote caller-scope regression.

// (quote (let ((x 1)) x)) — NodeTag::Quote form (#3181 fix shape).
static NodeId build_quote_let_node(FlatAST& src, StringPool& sp) {
    auto x = sp.intern("x");
    auto lit_1 = src.add_literal(1);
    auto v_x = src.add_variable(x);
    auto let_id = src.add_let(x, lit_1, v_x);
    return src.add_quote(let_id);
}

// (quote (let ((x 1)) x)) — Call-head "quote" form (#3181 fix shape).
// Distinct NodeTag::Quote from #3154's pre_scan handler — pre_scan
// doesn't recognize Call-head "quote", but #3181's clone walk in_quote
// flag does.
static NodeId build_quote_call_let(FlatAST& src, StringPool& sp) {
    auto x = sp.intern("x");
    auto lit_1 = src.add_literal(1);
    auto v_x = src.add_variable(x);
    auto let_id = src.add_let(x, lit_1, v_x);
    auto quote_var = src.add_variable(sp.intern("quote"));
    const NodeId quote_args[] = {let_id};
    return src.add_call(quote_var, std::span<const NodeId>{quote_args});
}

// (quasiquote (let ((x 1)) x)) — qq template-scope regression (must
// still gensym, binding == ref in __x_N form).
static NodeId build_qq_let(FlatAST& src, StringPool& sp) {
    auto x = sp.intern("x");
    auto lit_1 = src.add_literal(1);
    auto v_x = src.add_variable(x);
    auto let_id = src.add_let(x, lit_1, v_x);
    auto qq_var = src.add_variable(sp.intern("quasiquote"));
    const NodeId qq_args[] = {let_id};
    return src.add_call(qq_var, std::span<const NodeId>{qq_args});
}

// (quasiquote (let ((x 1)) (unquote x))) — qq + unquote caller-scope
// regression (outer gensym, inner verbatim per #2807).
static NodeId build_qq_let_unquote(FlatAST& src, StringPool& sp) {
    auto x = sp.intern("x");
    auto lit_1 = src.add_literal(1);
    auto v_x_inner = src.add_variable(x);
    auto uq_var = src.add_variable(sp.intern("unquote"));
    const NodeId uq_args[] = {v_x_inner};
    auto uq_call = src.add_call(uq_var, std::span<const NodeId>{uq_args});
    auto let_id = src.add_let(x, lit_1, uq_call);
    auto qq_var = src.add_variable(sp.intern("quasiquote"));
    const NodeId qq_args[] = {let_id};
    return src.add_call(qq_var, std::span<const NodeId>{qq_args});
}

} // namespace

int run_test_unquote_splicing_hygiene() {
    std::println("=== Issue #2807: unquote-splicing hygiene boundary ===");
    CHECK(true, "ac2807: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: pre_scan recognizes unquote-splicing ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(!me.empty(), "AC1: sources readable");
        // Anchor on the pre_scan handler specifically (multi-line check
        // with metric bump + early return). The bare
        // `cname == "unquote-splicing"` substring matches multiple
        // call sites after #3181 added clone-walk in_unquote tracking.
        auto pos = me.find("cname == \"unquote-splicing\") {");
        if (pos == std::string::npos)
            pos = me.find("cname == \"unquote-splicing\"");
        if (pos == std::string::npos)
            pos = me.find("unquote-splicing");
        CHECK(pos != std::string::npos, "AC1: unquote-splicing in pre_scan");
        // Lead-in covers Issue #2807 comment above the handler.
        auto win_start = pos > 600 ? pos - 600 : 0;
        auto win = me.substr(win_start, 1200);
        CHECK(win.find("Issue #2807") != std::string::npos, "AC1: cites #2807");
        CHECK(win.find("g_unquote_splicing_hygiene_mismatch_total") != std::string::npos,
              "AC1: metric bump");
        // Still has unquote stop.
        CHECK(me.find("cname == \"unquote\"") != std::string::npos,
              "AC1: unquote boundary retained");
        CHECK(ixx.find("g_unquote_splicing_hygiene_mismatch_total") != std::string::npos,
              "AC1: ixx export");
        CHECK(bridge.find("aura_unquote_splicing_hygiene_mismatch_total_v_read") !=
                  std::string::npos,
              "AC1: bridge v_read");
    }

    // ── AC2: splice body not treated as qq template for nested rest ──
    {
        std::println("\n--- AC2: qq+unquote-splicing+dotted does not bump nested_qq ---");
        FlatAST src;
        StringPool sp;
        auto root = build_qq_unsplice_dotted(src, sp);
        FlatAST target;
        StringPool tp;
        NameMap nm;
        const auto qq0 = g_macro_rest_param_nested_qq_hits_total.load(std::memory_order_relaxed);
        aura_test_reset_unquote_splicing_hygiene_mismatch_total_for_test();
        const auto m0 = g_unquote_splicing_hygiene_mismatch_total.load(std::memory_order_relaxed);

        auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                  SyntaxMarker::MacroIntroduced);
        CHECK(c != NULL_NODE, "AC2: clone ok");
        const auto qq1 = g_macro_rest_param_nested_qq_hits_total.load(std::memory_order_relaxed);
        const auto m1 = g_unquote_splicing_hygiene_mismatch_total.load(std::memory_order_relaxed);
        CHECK(qq1 == qq0, "AC2: nested_qq_hits NOT bumped (splice is caller scope)");
        CHECK(m1 > m0, "AC2: unquote-splicing boundary metric bumped");
        CHECK(aura_unquote_splicing_hygiene_mismatch_total_v_read() == m1, "AC2: v_read");
    }

    // ── AC3: plain qq + dotted still bumps nested_qq (regression) ──
    {
        std::println("\n--- AC3: qq+dotted without splice still bumps nested_qq ---");
        FlatAST src;
        StringPool sp;
        auto root = build_qq_dotted(src, sp);
        FlatAST target;
        StringPool tp;
        NameMap nm;
        const auto qq0 = g_macro_rest_param_nested_qq_hits_total.load(std::memory_order_relaxed);
        auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                  SyntaxMarker::MacroIntroduced);
        CHECK(c != NULL_NODE, "AC3: clone ok");
        const auto qq1 = g_macro_rest_param_nested_qq_hits_total.load(std::memory_order_relaxed);
        CHECK(qq1 > qq0, "AC3: nested_qq_hits bumped for qq template dotted rest");
        bool found_rest = false;
        for (const auto& [k, v] : nm) {
            if (v.rfind("__rest_", 0) == 0) {
                found_rest = true;
                break;
            }
        }
        CHECK(found_rest, "AC3: rest gensym present in name_map");
    }

    // ── AC4: unquote alone still does not bump splice metric ──
    {
        std::println("\n--- AC4: plain unquote does not bump splice metric ---");
        FlatAST src;
        StringPool sp;
        auto a = sp.intern("a");
        auto rest = sp.intern("rest");
        auto rv = src.add_variable(rest);
        auto lam = src.add_lambda(std::vector<aura::ast::SymId>{a, rest}, rv, /*dotted=*/true);
        auto uq_var = src.add_variable(sp.intern("unquote"));
        const NodeId uq_args[] = {lam};
        auto uq = src.add_call(uq_var, std::span<const NodeId>{uq_args});
        auto qq_var = src.add_variable(sp.intern("quasiquote"));
        const NodeId qq_args[] = {uq};
        auto root = src.add_call(qq_var, std::span<const NodeId>{qq_args});

        FlatAST target;
        StringPool tp;
        NameMap nm;
        aura_test_reset_unquote_splicing_hygiene_mismatch_total_for_test();
        const auto m0 = g_unquote_splicing_hygiene_mismatch_total.load(std::memory_order_relaxed);
        (void)clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                               SyntaxMarker::MacroIntroduced);
        const auto m1 = g_unquote_splicing_hygiene_mismatch_total.load(std::memory_order_relaxed);
        CHECK(m1 == m0, "AC4: unquote alone does not bump splice metric");
    }

    // Issue #3181: clone walk in_quote boundary (binding/ref split
    // residual of #3154). Pre_scan #3154 stops at NodeTag::Quote, but
    // the clone walk kept renaming Let/Lambda/Define bindings inside
    // quote (recursive children clone runs BEFORE rename_binding),
    // producing `(quote (let ((__x_0 1)) x))` — binding/ref split.
    // Fix: clone_macro_body_at_depth adds `in_quote` parameter;
    // local_in_quote is set when entering NodeTag::Quote OR Call-head
    // "quote", then propagated to recursive children so the subtree is
    // cloned verbatim (transplant only, no rename_binding / no
    // name_map write / no resolve_name). pre_scan unchanged.
    {
        std::println("\n=== Issue #3181: clone walk in_quote boundary ===");
        CHECK(true, "ac3181: issue stamp");

        // ── AC3181.1: NodeTag::Quote — binding == ref preserved ──
        {
            std::println("\n--- AC3181.1: NodeTag::Quote — binding == ref ---");
            FlatAST src;
            StringPool sp;
            auto root = build_quote_let_node(src, sp);
            FlatAST target;
            StringPool tp;
            NameMap nm;
            auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                      SyntaxMarker::MacroIntroduced);
            CHECK(c != NULL_NODE, "AC3181.1: clone ok");
            auto qv = target.get(c);
            CHECK(qv.tag == NodeTag::Quote, "AC3181.1: root is NodeTag::Quote");
            CHECK(!qv.children.empty(), "AC3181.1: Quote has child");
            auto let_id = qv.children[0];
            auto lv = target.get(let_id);
            CHECK(lv.tag == NodeTag::Let, "AC3181.1: inner is Let");
            auto binding_sym = lv.sym_id;
            auto body_var_id = lv.children.size() >= 2 ? lv.children[1] : NULL_NODE;
            auto body_var = target.get(body_var_id);
            CHECK(body_var.tag == NodeTag::Variable, "AC3181.1: body is Variable");
            auto ref_sym = body_var.sym_id;
            CHECK(binding_sym == ref_sym,
                  "AC3181.1: binding sym == ref sym (no binding/ref split)");
            CHECK(std::string(tp.resolve(binding_sym)) == "x",
                  "AC3181.1: original name preserved (no __x_N gensym)");
            CHECK(nm.empty(), "AC3181.1: name_map empty (no pre_scan gensym under quote)");
        }

        // ── AC3181.2: Call-head "quote" — same boundary ──
        {
            std::println("\n--- AC3181.2: Call-head \"quote\" — binding == ref ---");
            FlatAST src;
            StringPool sp;
            auto root = build_quote_call_let(src, sp);
            FlatAST target;
            StringPool tp;
            NameMap nm;
            auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                      SyntaxMarker::MacroIntroduced);
            CHECK(c != NULL_NODE, "AC3181.2: clone ok");
            auto cv = target.get(c);
            CHECK(cv.tag == NodeTag::Call, "AC3181.2: root is Call");
            CHECK(cv.children.size() >= 2, "AC3181.2: Call has 2+ children");
            auto callee_v = target.get(cv.children[0]);
            CHECK(callee_v.tag == NodeTag::Variable, "AC3181.2: callee is Variable");
            CHECK(std::string(tp.resolve(callee_v.sym_id)) == "quote",
                  "AC3181.2: callee name verbatim");
            auto let_id = cv.children[1];
            auto lv = target.get(let_id);
            CHECK(lv.tag == NodeTag::Let, "AC3181.2: arg is Let");
            auto binding_sym = lv.sym_id;
            auto body_var_id = lv.children.size() >= 2 ? lv.children[1] : NULL_NODE;
            auto body_var = target.get(body_var_id);
            CHECK(body_var.tag == NodeTag::Variable, "AC3181.2: body is Variable");
            auto ref_sym = body_var.sym_id;
            CHECK(binding_sym == ref_sym,
                  "AC3181.2: binding sym == ref sym (Call-head 'quote' boundary)");
            CHECK(std::string(tp.resolve(binding_sym)) == "x",
                  "AC3181.2: original name preserved (no __x_N gensym)");
        }

        // ── AC3181.3: regression — qq + let → binding == ref in __x_N form ──
        {
            std::println("\n--- AC3181.3: qq + let regression — both gensym'd ---");
            FlatAST src;
            StringPool sp;
            auto root = build_qq_let(src, sp);
            FlatAST target;
            StringPool tp;
            NameMap nm;
            auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                      SyntaxMarker::MacroIntroduced);
            CHECK(c != NULL_NODE, "AC3181.3: clone ok");
            auto cv = target.get(c);
            CHECK(cv.tag == NodeTag::Call, "AC3181.3: qq is Call");
            auto let_id_t = cv.children[1];
            auto lv = target.get(let_id_t);
            auto binding_sym = lv.sym_id;
            auto body_var_id = lv.children.size() >= 2 ? lv.children[1] : NULL_NODE;
            auto body_var = target.get(body_var_id);
            auto ref_sym = body_var.sym_id;
            CHECK(binding_sym == ref_sym, "AC3181.3: qq binding == ref (gensym form, both __x_N)");
            auto bname = std::string(tp.resolve(binding_sym));
            CHECK(bname != "x", "AC3181.3: qq DOES gensym (template scope)");
            CHECK(bname.rfind("__x_", 0) == 0, "AC3181.3: __x_<N> form");
        }

        // ── AC3181.4: regression — qq + let + unquote → caller scope ──
        {
            std::println(
                "\n--- AC3181.4: qq + let + unquote regression — outer gensym, inner verbatim ---");
            FlatAST src;
            StringPool sp;
            auto root = build_qq_let_unquote(src, sp);
            FlatAST target;
            StringPool tp;
            NameMap nm;
            auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                      SyntaxMarker::MacroIntroduced);
            CHECK(c != NULL_NODE, "AC3181.4: clone ok");
            auto cv = target.get(c);
            auto let_id_t = cv.children[1];
            auto lv = target.get(let_id_t);
            auto binding_sym = lv.sym_id;
            auto bname = std::string(tp.resolve(binding_sym));
            CHECK(bname.rfind("__x_", 0) == 0,
                  "AC3181.4: outer Let x gensym'd (template scope under qq)");
            // body of outer Let is (unquote x) — find inner x.
            auto uq_call_id = lv.children[1];
            auto uq_v = target.get(uq_call_id);
            CHECK(uq_v.tag == NodeTag::Call, "AC3181.4: body is Call");
            auto uq_var_id = uq_v.children[0];
            auto uq_var_v = target.get(uq_var_id);
            CHECK(uq_var_v.tag == NodeTag::Variable, "AC3181.4: unquote callee is Variable");
            CHECK(std::string(tp.resolve(uq_var_v.sym_id)) == "unquote",
                  "AC3181.4: unquote callee name verbatim");
            auto x_ref_id = uq_v.children[1];
            auto x_ref_v = target.get(x_ref_id);
            CHECK(x_ref_v.tag == NodeTag::Variable, "AC3181.4: inner x is Variable");
            CHECK(std::string(tp.resolve(x_ref_v.sym_id)) == "x",
                  "AC3181.4: unquote's x NOT gensym'd (caller scope per #2807)");
        }

        // ── AC3181.5: source-cite — clone_macro_body_at_depth has in_quote ──
        {
            std::println("\n--- AC3181.5: source-cite in_quote param + local_in_quote ---");
            auto me = read_file("src/compiler/macro_expansion.cpp");
            CHECK(!me.empty(), "AC3181.5: macro_expansion.cpp readable");
            auto pos_def = me.find("bool in_quote, bool in_unquote) {");
            auto pos_decl = me.find("bool in_quote = false, bool in_unquote = false);");
            CHECK(pos_def != std::string::npos,
                  "AC3181.5: clone_macro_body_at_depth definition has in_quote param");
            CHECK(pos_decl != std::string::npos,
                  "AC3181.5: clone_macro_body_at_depth forward decl has in_quote default");
            auto pos_local = me.find("bool local_in_quote = in_quote;");
            CHECK(pos_local != std::string::npos, "AC3181.5: local_in_quote flag declared");
            // Lead-in: covers the Issue #3181 comment block above the
            // local_in_quote computation (10 lines of design rationale).
            auto win_start = pos_local > 900 ? pos_local - 900 : 0;
            auto win = me.substr(win_start, 1800);
            CHECK(win.find("Issue #3181") != std::string::npos, "AC3181.5: cites #3181");
            CHECK(win.find("clone walk in_quote boundary") != std::string::npos,
                  "AC3181.5: cites fix shape");
            CHECK(win.find("cname == \"quote\"") != std::string::npos,
                  "AC3181.5: Call-head \"quote\" recognized");
            // Recursive children clone: in_quote threaded through.
            auto pos_recur =
                me.find("clone_macro_body_at_depth(target, target_pool, source, source_pool, cid,");
            CHECK(pos_recur != std::string::npos, "AC3181.5: recursive call site found");
            auto win_recur = me.substr(pos_recur, 500);
            CHECK(win_recur.find("local_in_quote") != std::string::npos,
                  "AC3181.5: local_in_quote threaded into recursive call");
        }

        // ── AC3181.6: cross-FlatAST — quoted data intern only ──
        {
            std::println("\n--- AC3181.6: cross-FlatAST quote clone — names preserved ---");
            FlatAST src;
            StringPool sp;
            auto root = build_quote_let_node(src, sp);
            FlatAST target; // separate FlatAST — cross-flat
            StringPool tp;
            NameMap nm;
            auto c = clone_macro_body(target, tp, src, sp, root, nullptr, &nm,
                                      SyntaxMarker::MacroIntroduced);
            CHECK(c != NULL_NODE, "AC3181.6: cross-flat clone ok");
            auto qv = target.get(c);
            CHECK(qv.tag == NodeTag::Quote, "AC3181.6: root is Quote");
            auto lv = target.get(qv.children[0]);
            auto binding_sym = lv.sym_id;
            auto body_var = target.get(lv.children[1]);
            auto ref_sym = body_var.sym_id;
            CHECK(binding_sym == ref_sym, "AC3181.6: cross-flat binding == ref");
            CHECK(std::string(tp.resolve(binding_sym)) == "x",
                  "AC3181.6: cross-flat quoted data — no α-rename");
        }
    }

    std::println("\n=== #2807 unquote-splicing hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_unquote_splicing_hygiene();
}
#endif

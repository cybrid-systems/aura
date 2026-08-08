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
        auto pos = me.find("cname == \"unquote-splicing\"");
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

    std::println("\n=== #2807 unquote-splicing hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_unquote_splicing_hygiene();
}
#endif

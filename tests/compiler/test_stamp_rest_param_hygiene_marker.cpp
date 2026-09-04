// @category: unit
// @reason: Issue #2808 — stamp_rest_param_hygiene must set
// SyntaxMarker::MacroIntroduced so is_macro_introduced() hygiene gates
// see rest-list spines (parity with clone_macro_body marker path).
//
//   AC1: stamp_rest_param_hygiene cites #2808; set_marker MacroIntroduced
//   AC2: stamp unmarked list → is_macro_introduced + set metric
//   AC3: re-stamp already-MacroIntroduced → skipped metric
//   AC4: this suite + linter; no docs/design/2808-*; no test_issue_2808.cpp

#include "test_harness.hpp"

#include <array>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

#include "compiler/aura_jit_bridge.h"

import std;
import aura.compiler.macro_expansion;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::macro_exp::g_stamp_rest_param_marker_set_total;
using aura::compiler::macro_exp::g_stamp_rest_param_marker_skipped_total;
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

} // namespace

int run_test_stamp_rest_param_hygiene_marker() {
    std::println("=== Issue #2808: stamp_rest_param_hygiene MacroIntroduced marker ===");
    CHECK(true, "ac2808: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: set_marker MacroIntroduced in stamp_rest_param_hygiene ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(!me.empty(), "AC1: sources readable");
        auto pos = me.find("inline void stamp_rest_param_hygiene");
        CHECK(pos != std::string::npos, "AC1: stamp_rest_param_hygiene present");
        auto win = me.substr(pos, 2500);
        CHECK(win.find("Issue #2808") != std::string::npos, "AC1: cites #2808");
        CHECK(win.find("set_marker") != std::string::npos, "AC1: set_marker");
        CHECK(win.find("MacroIntroduced") != std::string::npos, "AC1: MacroIntroduced");
        CHECK(win.find("g_stamp_rest_param_marker_set_total") != std::string::npos,
              "AC1: set metric");
        CHECK(win.find("g_stamp_rest_param_marker_skipped_total") != std::string::npos,
              "AC1: skipped metric");
        CHECK(ixx.find("g_stamp_rest_param_marker_set_total") != std::string::npos,
              "AC1: ixx set total");
        CHECK(bridge.find("aura_stamp_rest_param_marker_set_total_v_read") != std::string::npos,
              "AC1: bridge v_read");
        CHECK(bridge.find("aura_test_call_stamp_rest_param_hygiene") != std::string::npos,
              "AC1: test entry");
    }

    // ── AC2: unmarked spine becomes MacroIntroduced ──
    {
        std::println("\n--- AC2: stamp sets MacroIntroduced on rest list ---");
        FlatAST src;
        StringPool sp;
        auto body = src.add_literal(static_cast<std::int64_t>(0));
        src.set_schema_cache(body, 0x2808u);
        src.root = body;

        FlatAST target;
        StringPool tp;
        auto list_var = target.add_variable(tp.intern("list"));
        auto a1 = target.add_variable(tp.intern("a"));
        auto a2 = target.add_variable(tp.intern("b"));
        const std::array<aura::ast::NodeId, 2> args = {a1, a2};
        auto list_call = target.add_call(list_var, std::span<const aura::ast::NodeId>{args});
        CHECK(!target.is_macro_introduced(list_call), "AC2: list_call unmarked before");
        CHECK(!target.is_macro_introduced(a1), "AC2: a1 unmarked before");

        aura_test_reset_stamp_rest_param_marker_totals_for_test();
        const auto set0 = g_stamp_rest_param_marker_set_total.load();
        const auto skip0 = g_stamp_rest_param_marker_skipped_total.load();

        aura_test_call_stamp_rest_param_hygiene(
            static_cast<void*>(&target), static_cast<void*>(&src), static_cast<std::uint32_t>(body),
            static_cast<std::uint32_t>(list_call));

        CHECK(target.is_macro_introduced(list_call), "AC2: list_call MacroIntroduced");
        CHECK(target.is_macro_introduced(list_var), "AC2: list_var MacroIntroduced");
        CHECK(!target.is_macro_introduced(a1), "3468: remaining a1 keeps User marker");
        CHECK(!target.is_macro_introduced(a2), "3468: remaining a2 keeps User marker");
        const auto set1 = g_stamp_rest_param_marker_set_total.load();
        const auto skip1 = g_stamp_rest_param_marker_skipped_total.load();
        CHECK(set1 > set0, "AC2: set metric advanced");
        CHECK(set1 - set0 == 2, "3468: spine only (list_call + list_var)");
        CHECK(skip1 == skip0, "AC2: no skips on first stamp");
        CHECK(aura_stamp_rest_param_marker_set_total_v_read() == set1, "AC2: v_read set");
        CHECK((target.macro_dirty(list_call) &
               static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion)) !=
                  0,
              "AC2: kMacroExpansion dirty retained");
    }

    // ── AC3: already-marked → skipped ──
    {
        std::println("\n--- AC3: re-stamp skips already MacroIntroduced ---");
        FlatAST src;
        StringPool sp;
        auto body = src.add_literal(static_cast<std::int64_t>(1));
        FlatAST target;
        StringPool tp;
        auto list_var = target.add_variable(tp.intern("list"));
        auto a1 = target.add_variable(tp.intern("x"));
        const std::array<aura::ast::NodeId, 1> args = {a1};
        auto list_call = target.add_call(list_var, std::span<const aura::ast::NodeId>{args});
        // Pre-mark whole spine.
        target.set_marker(list_call, SyntaxMarker::MacroIntroduced);
        target.set_marker(list_var, SyntaxMarker::MacroIntroduced);
        target.set_marker(a1, SyntaxMarker::MacroIntroduced);

        aura_test_reset_stamp_rest_param_marker_totals_for_test();
        const auto set0 = g_stamp_rest_param_marker_set_total.load();
        const auto skip0 = g_stamp_rest_param_marker_skipped_total.load();
        aura_test_call_stamp_rest_param_hygiene(
            static_cast<void*>(&target), static_cast<void*>(&src), static_cast<std::uint32_t>(body),
            static_cast<std::uint32_t>(list_call));
        const auto set1 = g_stamp_rest_param_marker_set_total.load();
        const auto skip1 = g_stamp_rest_param_marker_skipped_total.load();
        CHECK(set1 == set0, "AC3: set metric unchanged on re-stamp");
        CHECK(skip1 > skip0, "AC3: skipped metric advanced");
        CHECK(target.is_macro_introduced(list_call), "AC3: still MacroIntroduced");
        CHECK(aura_stamp_rest_param_marker_skipped_total_v_read() == skip1, "AC3: v_read skip");
    }

    // ── #3468: pair spine stamps pair cells, never car (caller args) ──
    {
        std::println("\n--- #3468: pair spine does not stamp remaining cars ---");
        FlatAST src;
        auto body = src.add_literal(static_cast<std::int64_t>(2));
        FlatAST target;
        StringPool tp;
        auto a1 = target.add_variable(tp.intern("a"));
        auto a2 = target.add_variable(tp.intern("b"));
        auto p0 = target.add_pair(a2, NULL_NODE);
        auto p1 = target.add_pair(a1, p0);
        CHECK(!target.is_macro_introduced(p1), "3468: pair root unmarked before");
        CHECK(!target.is_macro_introduced(a1), "3468: car unmarked before");
        aura_test_reset_stamp_rest_param_marker_totals_for_test();
        aura_test_call_stamp_rest_param_hygiene(
            static_cast<void*>(&target), static_cast<void*>(&src), static_cast<std::uint32_t>(body),
            static_cast<std::uint32_t>(p1));
        CHECK(target.is_macro_introduced(p1), "3468: pair root MacroIntroduced");
        CHECK(target.is_macro_introduced(p0), "3468: cdr pair cell MacroIntroduced");
        CHECK(!target.is_macro_introduced(a1), "3468: remaining car a1 stays User");
        CHECK(!target.is_macro_introduced(a2), "3468: remaining car a2 stays User");
        const auto me = read_file("src/compiler/macro_expansion.cpp");
        CHECK(me.find("Issue #3468") != std::string::npos, "3468: helper cites #3468");
        CHECK(me.find("never DFS") != std::string::npos ||
                  me.find("never remaining caller") != std::string::npos,
              "3468: helper refuses remaining walk");
        CHECK(read_file("docs/design/3468-rest-overstamp.md").empty(),
              "3468: no docs/design/3468-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3468.cpp").empty(),
              "3468: no test_issue_3468.cpp per #81967");
    }

    std::println("\n=== #2808 stamp_rest_param_hygiene marker: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_stamp_rest_param_hygiene_marker();
}
#endif

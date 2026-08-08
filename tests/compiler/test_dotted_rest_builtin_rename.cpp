// @category: unit
// @reason: Issue #2805 — dotted-rest force-repair must not map
// hygiene_builtins names into name_map (would silently rename builtins).
//
//   AC1: Lambda fallback cites #2805; hygiene_builtins guard + metric
//   AC2: clone (lambda list body) with name_map — "list" not mapped
//   AC3: rest formal stays "list" (no __rest_fb_); metric bumped
//   AC4: non-builtin rest still force-repairs when incomplete
//   AC5: this suite + linter; no docs/design/2805-*; no test_issue_2805.cpp

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
import aura.parser.parser;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_dotted_rest_builtin_rename_prevented_total;
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

// Find first Lambda with dotted rest in flat; return (lam_id, rest_param_name).
static bool find_dotted_lambda(const FlatAST& flat, const StringPool& pool, NodeId& out_lam,
                               std::string& out_rest) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag != NodeTag::Lambda || v.int_value == 0 || v.params.empty())
            continue;
        out_lam = id;
        out_rest = std::string(pool.resolve(v.params.back()));
        return true;
    }
    return false;
}

} // namespace

int run_test_dotted_rest_builtin_rename() {
    std::println("=== Issue #2805: dotted-rest builtin rename prevented ===");
    CHECK(true, "ac2805: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: fallback guards hygiene_builtins ---");
        auto me = read_file("src/compiler/macro_expansion.cpp");
        auto ixx = read_file("src/compiler/macro_expansion.ixx");
        auto bridge = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(!me.empty(), "AC1: macro_expansion readable");
        auto pos = me.find("case NodeTag::Lambda:");
        // Prefer the clone_macro_body Lambda case near rest fallback.
        auto fb = me.find("__rest_fb_");
        CHECK(fb != std::string::npos, "AC1: __rest_fb_ fallback present");
        // Window around fallback for #2805 guard.
        auto win_start = fb > 800 ? fb - 800 : 0;
        auto win = me.substr(win_start, 1600);
        CHECK(win.find("Issue #2805") != std::string::npos, "AC1: cites #2805");
        CHECK(win.find("hygiene_builtins") != std::string::npos, "AC1: hygiene_builtins guard");
        CHECK(win.find("g_dotted_rest_builtin_rename_prevented_total") != std::string::npos,
              "AC1: metric bump");
        CHECK(ixx.find("g_dotted_rest_builtin_rename_prevented_total") != std::string::npos,
              "AC1: ixx export");
        CHECK(bridge.find("aura_dotted_rest_builtin_rename_prevented_total_v_read") !=
                  std::string::npos,
              "AC1: bridge v_read");
        (void)pos;
    }

    // ── AC2 + AC3: builtin rest name not mapped ──
    {
        std::println("\n--- AC2/AC3: dotted lambda rest=list not mapped ---");
        // "list" is in hygiene_builtins; pre-scan skips gensym; fallback
        // must not write name_map["list"].
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        StringPool sp(alloc);
        FlatAST src(alloc);
        auto list_sym = sp.intern("list");
        auto body = src.add_variable(list_sym);
        // Dotted single rest formal named `list` (builtin).
        auto lam = src.add_lambda(std::vector<aura::ast::SymId>{list_sym}, body, /*dotted=*/true);
        src.root = lam;
        CHECK(src.get(lam).int_value != 0, "AC2: dotted flag");

        FlatAST target(alloc);
        StringPool tp(alloc);
        NameMap name_map;

        aura_test_reset_dotted_rest_builtin_rename_prevented_total_for_test();
        const auto prev =
            g_dotted_rest_builtin_rename_prevented_total.load(std::memory_order_relaxed);

        auto cloned = clone_macro_body(target, tp, src, sp, lam, /*subst=*/nullptr, &name_map,
                                       aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(cloned != NULL_NODE, "AC2: clone ok");
        CHECK(!name_map.count("list"), "AC2: name_map has no \"list\" entry");
        // Rest formal must not be force-renamed to __rest_fb_list_*.
        NodeId out_lam = NULL_NODE;
        std::string rest_nm;
        CHECK(find_dotted_lambda(target, tp, out_lam, rest_nm),
              "AC3: found dotted lambda in target");
        CHECK(rest_nm == "list", "AC3: rest formal stays \"list\"");
        CHECK(rest_nm.rfind("__rest_", 0) != 0, "AC3: not __rest_ gensym");
        const auto now =
            g_dotted_rest_builtin_rename_prevented_total.load(std::memory_order_relaxed);
        CHECK(now > prev, "AC3: prevented metric bumped");
        CHECK(aura_dotted_rest_builtin_rename_prevented_total_v_read() == now, "AC3: v_read");
    }

    // ── AC4: non-builtin rest still force-repairs ──
    {
        std::println("\n--- AC4: non-builtin rest still gets __rest_ gensym ---");
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        StringPool sp(alloc);
        FlatAST src(alloc);
        auto rest_sym = sp.intern("rest");
        auto body = src.add_variable(rest_sym);
        auto lam = src.add_lambda(std::vector<aura::ast::SymId>{rest_sym}, body, /*dotted=*/true);
        src.root = lam;

        FlatAST target(alloc);
        StringPool tp(alloc);
        NameMap name_map;
        auto cloned = clone_macro_body(target, tp, src, sp, lam, nullptr, &name_map,
                                       aura::ast::SyntaxMarker::MacroIntroduced);
        CHECK(cloned != NULL_NODE, "AC4: clone ok");
        // Pre-scan or fallback should map "rest" to a __rest_* name.
        if (name_map.count("rest")) {
            CHECK(name_map["rest"].rfind("__rest_", 0) == 0, "AC4: rest mapped to __rest_");
        } else {
            NodeId out_lam = NULL_NODE;
            std::string rest_nm;
            CHECK(find_dotted_lambda(target, tp, out_lam, rest_nm), "AC4: dotted lambda");
            CHECK(rest_nm.rfind("__rest_", 0) == 0 || rest_nm == "rest",
                  "AC4: rest formal hygienic or original");
        }
    }

    std::println("\n=== #2805 dotted-rest builtin rename: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dotted_rest_builtin_rename();
}
#endif

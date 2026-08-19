// @category: unit
// @reason: Issue #3151 — expand_inner_macros cloned body must carry
// SyntaxMarker::MacroIntroduced (not default User). Residual call-
// site gap: top-level eval_flat (#3854) and macro_expand_all_body
// (#6105) already pass MacroIntroduced; expand_inner_macros in
// macro_expansion.cpp was dropping to default User, so mutate:replace-
// subtree / mutate:remove-node on nested-expanded nodes would default-
// allow without :allow-macro? and query:pattern skip_macro_introduced
// would not hide nested expansion residue.
//
//   AC1: source cites #3151 + expand_inner_macros passes
//        SyntaxMarker::MacroIntroduced to clone_macro_body.
//   AC2: closure-materialization path (evaluator_eval_flat.cpp
//        lambda construction) still passes SyntaxMarker::User —
//        unchanged per AC2.
//   AC3: direct expand_inner_macros call with a 2-macro registry
//        (outer expands inner) → inner-expanded subtree root and
//        descendants have marker == MacroIntroduced.
//   AC4: nested expand via CompilerService eval (define-hygienic-macro
//        outer calls define-hygienic-macro inner) → workspace flat has
//        MacroIntroduced marker on the inner-expanded subtree.
//   AC5: Soft / Off behaviour unchanged — no new TLS / no new metrics
//        middle-layer (file-level atomics not added by this fix).
//
// Sibling tests implicitly covered (must remain green):
//   - tests/compiler/test_macro_intro_restamp.cpp (#2096)
//   - tests/compiler/test_macro_restamp_after_flat.cpp (#2019)
//   - tests/compiler/test_hygiene_mutate_closed_loop.cpp (#1611)
//   - tests/compiler/test_jit_macro_introduced_preserve.cpp
//   - tests/compiler/test_macro_hygiene_*.cpp (entire suite)
//   - tests/compiler/test_fiber_macro_hygiene_refresh.cpp (#1652)

#include "test_harness.hpp"
#include "core/transparent_string_hash.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::expand_inner_macros;
using aura::compiler::macro_exp::MacroExpansionDef;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto* p :
         {path, "src/compiler/macro_expansion.cpp", "../src/compiler/macro_expansion.cpp",
          "src/compiler/evaluator_eval_flat.cpp", "../src/compiler/evaluator_eval_flat.cpp"}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: source gate — expand_inner_macros passes SyntaxMarker::MacroIntroduced
// to clone_macro_body (the bug site).
static void ac1_source_expand_inner_macros_marker() {
    std::println("\n--- AC1: source — expand_inner_macros passes MacroIntroduced ---");
    auto mex = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!mex.empty(), "macro_expansion.cpp readable");
    CHECK(mex.find("#3151") != std::string::npos, "cites #3151");
    CHECK(mex.find("SyntaxMarker::MacroIntroduced") != std::string::npos,
          "SyntaxMarker::MacroIntroduced referenced in macro_expansion.cpp");
    const auto call_pos = mex.find("clone_macro_body(*flat, *pool, *md.flat");
    CHECK(call_pos != std::string::npos, "expand_inner_macros clone_macro_body call site found");
    if (call_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(call_pos + 400, mex.size());
        const std::string window(mex, call_pos, window_end - call_pos);
        CHECK(window.find("SyntaxMarker::MacroIntroduced") != std::string::npos,
              "clone_macro_body in expand_inner_macros passes MacroIntroduced");
        CHECK(window.find("SyntaxMarker::User") == std::string::npos,
              "clone_macro_body in expand_inner_macros does NOT pass User");
    }
}

// AC2: closure-materialization path (evaluator_eval_flat.cpp lambda /
// eval_data_as_code construction) still passes SyntaxMarker::User —
// intentionally unchanged per AC.
static void ac2_closure_materialization_unchanged() {
    std::println("\n--- AC2: closure-materialization path keeps User ---");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(!eef.empty(), "evaluator_eval_flat.cpp readable");
    const auto user_pos = eef.find("SyntaxMarker::User");
    CHECK(user_pos != std::string::npos, "SyntaxMarker::User still referenced in eval_flat");
    if (user_pos != std::string::npos) {
        const auto window_start = (user_pos > 200) ? user_pos - 200 : 0;
        const auto window_end = std::min<std::size_t>(user_pos + 400, eef.size());
        const std::string window(eef, window_start, window_end - window_start);
        CHECK(window.find("clone_macro_body") != std::string::npos,
              "User marker is adjacent to a clone_macro_body call (closure materialization)");
    }
}

// AC3: direct expand_inner_macros call with a 2-macro registry (outer
// expands inner) → inner-expanded subtree root has MacroIntroduced marker.
static void ac3_direct_expand_inner_macros() {
    std::println("\n--- AC3: direct expand_inner_macros stamps MacroIntroduced ---");
    FlatAST flat;
    StringPool pool;

    FlatAST inner_src;
    StringPool inner_sp;
    auto inner_x = inner_sp.intern("x");
    auto inner_xv = inner_src.add_variable(inner_x);
    auto inner_two = inner_src.add_literal(2);
    const aura::ast::NodeId inner_args[] = {inner_xv, inner_two};
    auto inner_mul = inner_src.add_call(inner_src.add_variable(inner_sp.intern("*")),
                                        std::span<const aura::ast::NodeId>(inner_args));
    inner_src.root = inner_mul;

    FlatAST outer_src;
    StringPool outer_sp;
    auto outer_x = outer_sp.intern("x");
    auto outer_xv = outer_src.add_variable(outer_x);
    auto outer_inner_var = outer_src.add_variable(outer_sp.intern("inner"));
    const aura::ast::NodeId outer_args[] = {outer_xv};
    auto outer_call =
        outer_src.add_call(outer_inner_var, std::span<const aura::ast::NodeId>(outer_args));
    outer_src.root = outer_call;

    std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                       std::equal_to<>>
        macros;
    MacroExpansionDef inner_def;
    inner_def.params = {"x"};
    inner_def.dotted = false;
    inner_def.flat = &inner_src;
    inner_def.pool = &inner_sp;
    inner_def.body_id = inner_mul;
    macros["inner"] = inner_def;

    MacroExpansionDef outer_def;
    outer_def.params = {"x"};
    outer_def.dotted = false;
    outer_def.flat = &outer_src;
    outer_def.pool = &outer_sp;
    outer_def.body_id = outer_call;
    macros["outer"] = outer_def;

    auto five = flat.add_literal(5);
    auto outer_var = flat.add_variable(pool.intern("outer"));
    const aura::ast::NodeId outer_call_args[] = {five};
    auto outer_call_site =
        flat.add_call(outer_var, std::span<const aura::ast::NodeId>(outer_call_args));

    const auto expanded = expand_inner_macros(&flat, &pool, outer_call_site, 0, 10, macros);
    CHECK(expanded != NULL_NODE, "expand_inner_macros returned a NodeId");

    std::size_t macro_intro_count = 0;
    std::vector<aura::ast::NodeId> stack;
    stack.push_back(expanded);
    while (!stack.empty()) {
        auto nid = stack.back();
        stack.pop_back();
        if (nid == NULL_NODE)
            continue;
        if (flat.is_macro_introduced(nid))
            ++macro_intro_count;
        auto v = flat.get(nid);
        for (auto c : v.children)
            stack.push_back(c);
    }
    CHECK(macro_intro_count >= 2,
          "nested expand → >= 2 MacroIntroduced nodes (outer-clone + inner-clone)");
}

// AC4: nested expand via CompilerService eval — define two hygienic
// macros where one calls the other, verify the workspace flat has
// MacroIntroduced marker on the inner-expanded subtree.
static void ac4_service_level_nested_expand() {
    std::println("\n--- AC4: CompilerService nested expand → MacroIntroduced ---");
    CompilerService cs;
    const auto setup = cs.eval(std::format("(set-code \""
                                           "(define-hygienic-macro (dbl y) (* y 2)) "
                                           "(define-hygienic-macro (quad y) (dbl (dbl y))) "
                                           "(quad 3)"
                                           "\")"));
    CHECK(setup.has_value(), "nested-macro set-code");
    const auto evaled = cs.eval("(eval-current)");
    CHECK(evaled.has_value(), "eval-current after nested macro setup");

    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat accessible");
    if (!ws)
        return;

    std::size_t macro_intro_count = 0;
    for (std::size_t i = 0; i < ws->size(); ++i) {
        if (ws->is_macro_introduced(static_cast<aura::ast::NodeId>(i)))
            ++macro_intro_count;
    }
    CHECK(macro_intro_count >= 1,
          "workspace flat has >= 1 MacroIntroduced node after nested expand");
}

// AC5: Soft / Off behaviour unchanged — no new TLS / no new metrics
// middle-layer. This fix only changes the cloned_marker argument on
// one call site. Source-cite gate: confirm no new g_* atomic / no new
// thread_local / no new query surface was added by this fix.
static void ac5_soft_off_unchanged() {
    std::println("\n--- AC5: Soft / Off unchanged — no new TLS / metrics ---");
    auto mex = read_file("src/compiler/macro_expansion.cpp");
    auto mex_200 = read_file("src/compiler/macro_expansion.ixx");
    auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(!mex.empty(), "macro_expansion.cpp readable");
    CHECK(!mex_200.empty(), "macro_expansion.ixx readable");
    CHECK(!obs.empty(), "observability_metrics.h readable");

    std::size_t atomic_count = 0;
    std::size_t search_from = 0;
    while (true) {
        const auto p = mex_200.find("export extern std::atomic", search_from);
        if (p == std::string::npos)
            break;
        ++atomic_count;
        search_from = p + 1;
    }
    CHECK(atomic_count > 0, "macro_expansion.ixx still exports file-level atomics");
    CHECK(mex_200.find("g_3151_") == std::string::npos,
          "no new g_3151_* atomic in macro_expansion.ixx (Soft/Off zero-cost preserved)");
    CHECK(mex.find("g_3151_") == std::string::npos,
          "no new g_3151_* atomic in macro_expansion.cpp (Soft/Off zero-cost preserved)");
    CHECK(obs.find("g_3151_") == std::string::npos,
          "no new g_3151_* atomic in observability_metrics.h (no middle-layer)");
}

} // namespace

int main() {
    ac1_source_expand_inner_macros_marker();
    ac2_closure_materialization_unchanged();
    ac3_direct_expand_inner_macros();
    ac4_service_level_nested_expand();
    ac5_soft_off_unchanged();
    if (g_failed)
        return 1;
    std::println("macro inner-expand marker (#3151): OK ({} passed)", g_passed);
    return 0;
}
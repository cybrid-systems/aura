// @category: unit
// @reason: Issue #1780 — *allow-macro-inline* must use per-Evaluator
// state (not InlinePass process-wide static). Concurrent CompilerServices
// / fibers must not clobber each other's macro-hygiene policy.
//
//   AC1: source cites #1780; uses set_inline_respect_macro_hygiene
//   AC2: no InlinePass::set/get_respect_macro_hygiene in primitive
//   AC3: two CompilerServices toggle independently
//   AC4: InlinePass hygiene is instance (not static member)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.pass_manager;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: per-Evaluator path (no InlinePass static) ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile_04.cpp",
                                "../src/compiler/evaluator_primitives_compile_04.cpp"});
        CHECK(!prim.empty(), "read compile_04.cpp");
        CHECK(prim.find("#1780") != std::string::npos, "cites #1780");
        auto pos = prim.find("add(\"*allow-macro-inline*\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 500);
        CHECK(win.find("set_inline_respect_macro_hygiene") != std::string::npos,
              "uses Evaluator setter");
        CHECK(win.find("get_inline_respect_macro_hygiene") != std::string::npos,
              "uses Evaluator getter");
        CHECK(win.find("InlinePass::set_respect_macro_hygiene") == std::string::npos,
              "no InlinePass static set in primitive");
        CHECK(win.find("InlinePass::get_respect_macro_hygiene") == std::string::npos,
              "no InlinePass static get in primitive");

        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!ixx.empty() && ixx.find("inline_respect_macro_hygiene_") != std::string::npos,
              "Evaluator stores per-eval policy");
        CHECK(ixx.find("set_inline_respect_macro_hygiene") != std::string::npos, "setter present");

        auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
        CHECK(!pm.empty(), "read pass_manager.ixx");
        CHECK(pm.find("static inline bool respect_macro_hygiene_") == std::string::npos,
              "InlinePass hygiene not process-wide static");
        CHECK(pm.find("bool respect_macro_hygiene_ = true") != std::string::npos,
              "InlinePass hygiene is instance field");
    }

    // ── AC3: independent CompilerServices ──
    {
        std::println("\n--- AC3: two services toggle independently ---");
        CompilerService a;
        CompilerService b;
        // Default: respect=true → inlinable flag returns 0 after (* #f) path;
        // enable #t → returns 1 (macro inline allowed).
        auto a_on = a.eval("(*allow-macro-inline* #t)");
        CHECK(a_on && is_int(*a_on) && as_int(*a_on) == 1, "A: enable → 1");
        // B still default (respect=true); enabling on A must not flip B.
        auto b_def = b.eval("(*allow-macro-inline* #f)");
        CHECK(b_def && is_int(*b_def) && as_int(*b_def) == 0, "B: disable → 0 (still default-ish)");
        // Re-assert A stayed enabled after B's toggle.
        auto a_again = a.eval("(*allow-macro-inline* #t)");
        CHECK(a_again && is_int(*a_again) && as_int(*a_again) == 1, "A still independent of B");

        CHECK(a.evaluator().get_inline_respect_macro_hygiene() == false,
              "A evaluator: respect=false after #t");
        CHECK(b.evaluator().get_inline_respect_macro_hygiene() == true,
              "B evaluator: respect=true after #f");
    }

    // ── AC4: InlinePass instances are independent ──
    {
        std::println("\n--- AC4: InlinePass instance policy isolation ---");
        aura::compiler::InlinePass p1;
        aura::compiler::InlinePass p2;
        CHECK(p1.get_respect_macro_hygiene() == true, "p1 default true");
        CHECK(p2.get_respect_macro_hygiene() == true, "p2 default true");
        p1.set_respect_macro_hygiene(false);
        CHECK(p1.get_respect_macro_hygiene() == false, "p1 set false");
        CHECK(p2.get_respect_macro_hygiene() == true, "p2 unaffected by p1");
    }

    std::println("\n=== test_allow_macro_inline_per_eval_1780: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

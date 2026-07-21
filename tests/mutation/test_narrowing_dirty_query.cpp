// @category: unit
// @reason: Issue #1779 — compile:narrowing-dirty? must use a read-only
// Issue #1779 (#1978 renamed): issue# moved from filename to header.
// query path (not set+restore via set_occurrence_dirty_fn_).
//
//   AC1: source cites #1779; query_occurrence_dirty_fn_ used
//   AC2: no set+restore pair in narrowing-dirty? primitive body
//   AC3: mark then peek → #t without clearing
//   AC4: concurrent mark while peeking cannot be undone by peek

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: query path (no set+restore) ---");
        std::string prim;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            prim = read_file(p);
            if (!prim.empty())
                break;
        }
        CHECK(!prim.empty(), "read compile_04.cpp");
        CHECK(prim.find("#1779") != std::string::npos, "cites #1779");
        auto pos = prim.find("compile:narrowing-dirty?");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 800);
        CHECK(win.find("query_occurrence_dirty_fn_") != std::string::npos, "uses query hook");
        CHECK(win.find("set_occurrence_dirty_fn_") == std::string::npos,
              "no set hook in query primitive body");

        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty() && ixx.find("query_occurrence_dirty_fn_") != std::string::npos,
              "Evaluator exports query hook");
        CHECK(ixx.find("set_query_occurrence_dirty_fn") != std::string::npos, "setter present");

        std::string svc;
        for (const char* p : {"src/compiler/service.ixx", "../src/compiler/service.ixx"}) {
            svc = read_file(p);
            if (!svc.empty())
                break;
        }
        CHECK(!svc.empty() && svc.find("set_query_occurrence_dirty_fn") != std::string::npos,
              "CompilerService wires query hook");
        CHECK(svc.find("#1779") != std::string::npos, "service cites #1779");
    }

    // ── AC3: mark + peek ──
    {
        std::println("\n--- AC3: mark then peek stays dirty ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code ok");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws && ws->size() > 0, "workspace non-empty");
        const auto target = static_cast<std::int64_t>(ws->size() - 1);
        auto set_r = cs.eval(std::format("(compile:mark-narrowing-dirty! {})", target));
        CHECK(set_r && is_bool(*set_r), "mark returns bool");
        auto peek1 = cs.eval(std::format("(compile:narrowing-dirty? {})", target));
        CHECK(peek1 && is_bool(*peek1) && as_bool(*peek1), "peek #t after mark");
        auto peek2 = cs.eval(std::format("(compile:narrowing-dirty? {})", target));
        CHECK(peek2 && is_bool(*peek2) && as_bool(*peek2),
              "second peek still #t (no restore clear)");
    }

    // ── AC4: many peeks never clear a marked bit ──
    {
        std::println("\n--- AC4: repeated peeks leave marked bit set ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code ok");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws && ws->size() > 0, "workspace non-empty");
        const auto target = static_cast<std::int64_t>(ws->size() - 1);
        (void)cs.eval(std::format("(compile:mark-narrowing-dirty! {})", target));
        for (int i = 0; i < 50; ++i) {
            auto p = cs.eval(std::format("(compile:narrowing-dirty? {})", target));
            CHECK(p && is_bool(*p) && as_bool(*p), "peek stays #t across iterations");
        }
    }

    std::println("\n=== test_narrowing_dirty_query_1779: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

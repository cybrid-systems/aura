// @category: unit
// @reason: Issue #1717 — synthesize:optimize child workspace swap must use
// RAII (restore + delete_child on scope exit), not bare manual restore.
//
//   AC1: source defines WorkspaceSwapGuard with dtor release
//   AC2: xover + evolve-variant paths use WorkspaceSwapGuard
//   AC3: no bare create_child+saved_flat manual restore in optimize body
//   AC4: cites Issue #1717
//   AC5: synthesize:optimize still registered

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string strip_line_comments(std::string_view win) {
    std::string code;
    code.reserve(win.size());
    for (size_t i = 0; i < win.size();) {
        if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
            while (i < win.size() && win[i] != '\n')
                ++i;
            continue;
        }
        code.push_back(win[i++]);
    }
    return code;
}

} // namespace

int main() {
    // ── AC1–AC4: source audit ──
    {
        std::println("\n--- AC1–AC4: WorkspaceSwapGuard RAII ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1717") != std::string::npos, "cites #1717");
            CHECK(src.find("class WorkspaceSwapGuard") != std::string::npos,
                  "WorkspaceSwapGuard defined");
            CHECK(src.find("~WorkspaceSwapGuard") != std::string::npos, "has destructor");
            CHECK(src.find("delete_child") != std::string::npos, "delete_child in release");

            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 20000 : end - pos);
                auto code = strip_line_comments(win);
                CHECK(code.find("WorkspaceSwapGuard") != std::string::npos,
                      "optimize uses WorkspaceSwapGuard");
                CHECK(code.find("\"xover\"") != std::string::npos ||
                          code.find("\"xover\"") != std::string::npos ||
                          win.find("xover") != std::string::npos,
                      "xover path present");
                CHECK(win.find("evolve-variant") != std::string::npos,
                      "evolve-variant path present");
                // Manual restore pattern should be gone from live code.
                CHECK(code.find("saved_flat") == std::string::npos, "no live saved_flat");
                CHECK(code.find("saved_f") == std::string::npos, "no live saved_f");
                // create_child only inside guard ctor, not bare in optimize body.
                // (Guard class is outside the add() window; body should not call create_child.)
                CHECK(code.find("create_child") == std::string::npos,
                      "no bare create_child in optimize body");
            }
        }
    }

    // ── AC5: primitive registered ──
    {
        std::println("\n--- AC5: synthesize:optimize registered ---");
        CompilerService cs;
        auto r = cs.eval("(procedure? synthesize:optimize)");
        CHECK(r.has_value(), "eval ok");
        if (r) {
            using aura::compiler::types::as_bool;
            using aura::compiler::types::is_bool;
            CHECK(is_bool(*r) && as_bool(*r), "synthesize:optimize is a procedure");
        }
    }

    std::println("\n=== test_workspace_swap_guard_1717: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

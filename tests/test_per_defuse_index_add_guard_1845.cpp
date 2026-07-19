// @category: unit
// @reason: Issue #1845 — compile:per-defuse-index-add must wrap
// tracker mutation in MutationBoundaryGuard + try/catch; document
// compiler_service_ non-owning ownership (#1839).
//
//   AC1: source cites #1845; Guard + try/catch around add_caller
//   AC2: add returns non-negative size under CompilerService
//   AC3: nested under outer Guard still completes

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
using aura::compiler::Evaluator;
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

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: Guard + try/catch on per-defuse-index-add ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_06.cpp",
                              "../src/compiler/evaluator_primitives_compile_06.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1845") != std::string::npos, "cites #1845");
        auto pos = src.find("\"compile:per-defuse-index-add\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 1800);
        CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("add_caller") != std::string::npos, "calls add_caller");
        CHECK(win.find("catch") != std::string::npos, "catch path");
        // Ownership note sits just above the add() site (#1839).
        auto pre = src.substr(pos > 400 ? pos - 400 : 0, 400);
        CHECK(pre.find("#1839") != std::string::npos ||
                  pre.find("non-owning") != std::string::npos ||
                  win.find("#1839") != std::string::npos,
              "documents service ownership");
    }

    // ── AC2: runtime ──
    {
        std::println("\n--- AC2: per-defuse-index-add returns size ---");
        CompilerService cs;
        auto r = cs.eval("(compile:per-defuse-index-add \"foo\" 0)");
        CHECK(r && is_int(*r), "returns int");
        CHECK(as_int(*r) >= 0, "size >= 0");
        auto r2 = cs.eval("(compile:per-defuse-index-add \"foo\" 1)");
        CHECK(r2 && is_int(*r2), "second add returns int");
        CHECK(as_int(*r2) >= as_int(*r), "size non-decreasing");
    }

    // ── AC3: nested Guard ──
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            auto r = cs.eval("(compile:per-defuse-index-add \"bar\" 3)");
            CHECK(r && is_int(*r) && as_int(*r) >= 0, "add under outer Guard ok");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer");
    }

    std::println("\n=== test_per_defuse_index_add_guard_1845: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

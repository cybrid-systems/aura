// @category: unit
// @reason: Issue #1816 — verify:coverage-holes /
// verify:suggest-constraint-refine must hold workspace_mtx_
// while reading (and, for report text, writing)
// verification_dirty_ so concurrent fibers cannot tear bits
// or race side-table realloc.
//
//   AC1: source cites #1816; shared/unique lock around dirty scan
//   AC2: both primitives take workspace_mtx_ (not lock-free scan)
//   AC3: coverage-holes with report marks + returns list (no hang)
//   AC4: interleaved parse + holes + suggest sequential smoke (no hang)

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
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
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
        std::println("\n--- AC1/AC2: workspace_mtx_ around verification_dirty scans ---");
        auto src = read_first({"src/compiler/evaluator_primitives_compile_04.cpp",
                               "../src/compiler/evaluator_primitives_compile_04.cpp"});
        CHECK(!src.empty(), "read compile_04.cpp");
        CHECK(src.find("#1816") != std::string::npos, "cites #1816");

        auto cov_pos = src.find("\"verify:coverage-holes\"");
        CHECK(cov_pos != std::string::npos, "coverage-holes primitive");
        auto cov_win = src.substr(cov_pos, 4200);
        CHECK(cov_win.find("workspace_mtx_") != std::string::npos, "coverage locks workspace_mtx_");
        CHECK(cov_win.find("shared_lock") != std::string::npos, "coverage uses shared_lock");
        CHECK(cov_win.find("unique_lock") != std::string::npos,
              "coverage uses unique_lock for report mark path");
        CHECK(cov_win.find("verification_dirty") != std::string::npos,
              "coverage reads dirty column");

        auto sug_pos = src.find("\"verify:suggest-constraint-refine\"");
        CHECK(sug_pos != std::string::npos, "suggest-constraint-refine primitive");
        auto sug_win = src.substr(sug_pos, 1200);
        CHECK(sug_win.find("workspace_mtx_") != std::string::npos, "suggest locks workspace_mtx_");
        CHECK(sug_win.find("shared_lock") != std::string::npos, "suggest uses shared_lock");
    }

    // ── AC3: report path marks + returns ──
    {
        std::println("\n--- AC3: coverage-holes report path ---");
        CompilerService cs;
        CHECK(cs.eval("(define x 1) (define y 2)").has_value(), "seed workspace");
        auto r = cs.eval("(verify:coverage-holes \"0 hole_a\\n1 hole_b\\n\")");
        CHECK(r.has_value(), "coverage-holes returns");
        // Empty workspace-size mismatch may yield void or pair list — must not hang/throw.
        CHECK(is_pair(*r) || is_void(*r), "coverage-holes returns pair-list or void");
        auto s = cs.eval("(verify:suggest-constraint-refine)");
        CHECK(s.has_value(), "suggest returns");
        CHECK(is_pair(*s) || is_void(*s), "suggest returns pair-list or void");
    }

    // ── AC4: interleaved parse + scan smoke ──
    {
        std::println("\n--- AC4: interleaved parse + holes + suggest ---");
        CompilerService cs;
        CHECK(cs.eval("(define a 1) (define b 2) (define c 3)").has_value(), "seed");
        for (int i = 0; i < 40; ++i) {
            auto p = cs.eval("(verify:parse-coverage-feedback \"0 hole\\n1 hole\\n\")");
            CHECK(p.has_value(), std::format("parse ok iter {}", i));
            auto h = cs.eval("(verify:coverage-holes)");
            CHECK(h.has_value(), std::format("holes ok iter {}", i));
            auto s = cs.eval("(verify:suggest-constraint-refine)");
            CHECK(s.has_value(), std::format("suggest ok iter {}", i));
        }
    }

    std::println("\n=== test_coverage_holes_workspace_lock_1816: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

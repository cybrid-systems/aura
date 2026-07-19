// @category: unit
// @reason: Issue #1842 — evaluator:compact-env-frames must wrap
// compact_env_frames() in MutationBoundaryGuard + try/catch so a
// mid-compact throw restores panic checkpoint (no partial env
// rewrite left committed).
//
//   AC1: source cites #1842; Guard + try/catch present
//   AC2: primitive returns int (reclaimed count) without hang
//   AC3: nested under outer Guard still completes (outermost lock)

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
        std::println("\n--- AC1: Guard + try/catch on compact-env-frames ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_07.cpp",
                              "../src/compiler/evaluator_primitives_compile_07.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1842") != std::string::npos, "cites #1842");
        auto pos = src.find("\"evaluator:compact-env-frames\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 1600);
        CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
              "try block");
        CHECK(win.find("catch") != std::string::npos, "catch path");
        CHECK(win.find("compact_env_frames()") != std::string::npos, "calls compact_env_frames");
    }

    // ── AC2: runtime ──
    {
        std::println("\n--- AC2: compact-env-frames returns int ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "seed eval");
        auto r = cs.eval("(evaluator:compact-env-frames)");
        CHECK(r && is_int(*r), "returns int");
        CHECK(as_int(*r) >= 0, "reclaimed >= 0 (or 0 empty)");
    }

    // ── AC3: nested Guard ──
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 2 2)").has_value(), "seed");
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            auto r = cs.eval("(evaluator:compact-env-frames)");
            CHECK(r && is_int(*r), "compact under outer Guard returns int");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held by outer");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
    }

    std::println("\n=== test_compact_env_frames_guard_1842: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #1853 — compile:clear-instruction-dirty! must wrap
// clear_instruction_dirty_fn_ in MutationBoundaryGuard + try/catch so
// a mid-clear throw restores panic checkpoint (subtractive dirty-bit
// clear must not leave partial IR cache state committed).
//
//   AC1: source cites #1853; Guard + try/catch present
//   AC2: without sandbox, clear returns bool (no hang)
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
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
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
        std::println("\n--- AC1: Guard + try/catch on clear-instruction-dirty! ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_03.cpp",
                              "../src/compiler/evaluator_primitives_compile_03.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_03.cpp");
        CHECK(src.find("#1853") != std::string::npos, "cites #1853");
        auto pos = src.find("\"compile:clear-instruction-dirty!\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2200);
        CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("clear_instruction_dirty_fn_") != std::string::npos,
              "calls clear_instruction_dirty_fn_");
        CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
              "try block");
        CHECK(win.find("catch") != std::string::npos, "catch path");
        // Capability gate remains (outside Guard).
        CHECK(win.find("kCapWildcard") != std::string::npos, "keeps capability gate");
    }

    // ── AC2: runtime (no sandbox — cap gate bypassed) ──
    {
        std::println("\n--- AC2: clear-instruction-dirty! returns bool ---");
        CompilerService cs;
        cs.evaluator().set_sandbox_mode(false);
        // Seed something so string heap / hooks may exist.
        (void)cs.eval("(define foo 1)");
        auto r = cs.eval(R"((compile:clear-instruction-dirty! "foo" 0 0 0))");
        CHECK(r.has_value(), "eval returns");
        if (r) {
            // bool (hook present or not) or not error.
            CHECK(is_bool(*r) || is_error(*r), "bool or error");
            if (is_bool(*r))
                CHECK(true, "returns bool under Guard");
        }
        // mark then clear sequential.
        auto m = cs.eval(R"((compile:mark-instruction-dirty! "foo" 0 0 0))");
        CHECK(m.has_value(), "mark eval ok");
        auto c = cs.eval(R"((compile:clear-instruction-dirty! "foo" 0 0 0))");
        CHECK(c.has_value() && is_bool(*c), "clear after mark returns bool");
    }

    // ── AC3: nested Guard ──
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        cs.evaluator().set_sandbox_mode(false);
        (void)cs.eval("(define bar 2)");
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            auto r = cs.eval(R"((compile:clear-instruction-dirty! "bar" 0 0 0))");
            CHECK(r.has_value() && is_bool(*r), "clear under outer Guard returns bool");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held by outer");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
    }

    std::println("\n=== test_clear_instruction_dirty_guard_1853: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

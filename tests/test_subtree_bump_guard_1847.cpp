// @category: unit
// @reason: Issue #1847 — compile:subtree-bump must wrap
// bump_generation_subtree in MutationBoundaryGuard + try/catch
// so a mid-ancestor-walk throw restores panic checkpoint
// (subtree_gen_ / generation_ not left partially consistent).
//
//   AC1: source cites #1847; Guard + try/catch present
//   AC2: bump on a real Define returns 0/1 without hang
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
        std::println("\n--- AC1: Guard + try/catch on compile:subtree-bump ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_06.cpp",
                              "../src/compiler/evaluator_primitives_compile_06.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1847") != std::string::npos, "cites #1847");
        auto pos = src.find("\"compile:subtree-bump\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 1800);
        CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("bump_generation_subtree") != std::string::npos,
              "calls bump_generation_subtree");
        CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
              "try block");
        CHECK(win.find("catch") != std::string::npos, "catch path");
    }

    // ── AC2: runtime ──
    {
        std::println("\n--- AC2: subtree-bump on workspace Define ---");
        CompilerService cs;
        // Load two defines so query:defines-by-marker yields ids.
        auto set = cs.eval("(set-code \"(define x 1) (define y 2)\")");
        CHECK(set.has_value(), "set-code ok");
        auto defs = cs.eval("(query:defines-by-marker \"User\")");
        CHECK(defs.has_value(), "defines-by-marker ok");
        // Bump first define's subtree (car of list).
        auto r = cs.eval("(compile:subtree-bump (car (query:defines-by-marker \"User\")))");
        CHECK(r.has_value(), "bump eval ok");
        if (r) {
            CHECK(is_int(*r), "returns int");
            // 1 = bumped, 0 = no-op, -1 = exception path under Guard.
            if (is_int(*r))
                CHECK(as_int(*r) >= 0, "success path (not -1)");
        }
        // Out-of-range id is a no-op (0), still under Guard.
        auto noop = cs.eval("(compile:subtree-bump 999999999)");
        CHECK(noop.has_value() && is_int(*noop) && as_int(*noop) == 0, "OOR id returns 0");
    }

    // ── AC3: nested Guard ──
    // Resolve the Define id *outside* the outer Guard: query
    // helpers may take shared_lock on workspace_mtx_, which
    // deadlocks with an exclusive outer MutationBoundaryGuard
    // (shared_mutex is not recursive).
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define z 3)\")").has_value(), "set-code seed");
        auto id_v = cs.eval("(car (query:defines-by-marker \"User\"))");
        CHECK(id_v && is_int(*id_v), "resolve define id outside Guard");
        const auto id = as_int(*id_v);
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            // Only the Guard-wrapped mutator under outer lock.
            auto r = cs.eval(std::format("(compile:subtree-bump {})", id));
            CHECK(r && is_int(*r) && as_int(*r) >= 0, "bump under outer Guard ok");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held by outer");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
    }

    std::println("\n=== test_subtree_bump_guard_1847: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

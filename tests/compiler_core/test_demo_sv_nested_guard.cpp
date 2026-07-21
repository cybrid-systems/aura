// @category: unit
// @reason: Issue #1774 — eda:demo-sv-self-evolution may call
// Issue #1774/#184/#236 (#1978 renamed): issue# moved from filename to header.
// eda:run-verification-feedback (own MutationBoundaryGuard) from an
// outer Guard without nested unique_lock deadlock (#184/#236: only
// outermost locks workspace_mtx_).
//
//   AC1: source cites #1774; documents nested-Guard contract
//   AC2: outer MutationBoundaryGuard + inner feedback call completes
//   AC3: demo primitive remains reachable (smoke)

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
        std::println("\n--- AC1: #1774 nested-Guard contract documented ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_04.cpp");
        CHECK(src.find("#1774") != std::string::npos, "cites #1774");
        CHECK(src.find("eda:demo-sv-self-evolution") != std::string::npos, "demo primitive");
        CHECK(src.find("nested unique_lock") != std::string::npos ||
                  src.find("outermost") != std::string::npos,
              "documents outermost-only lock contract");
        CHECK(src.find("eda:run-verification-feedback") != std::string::npos, "calls feedback_fn");
    }

    // ── AC2: outer Guard + feedback (nested Guard) ──
    {
        std::println("\n--- AC2: nested Guard via feedback under outer Guard ---");
        CompilerService cs;
        // Seed minimal workspace; feedback may return #f without SV nodes.
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code ok");
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            // Inner primitive opens its own Guard (depth 2) without re-lock.
            auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 hole\")");
            CHECK(r && is_bool(*r), "feedback returns bool under outer Guard");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth still held by outer");
        }
        CHECK(ok, "outer guard ok flag");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
    }

    // ── AC3: demo smoke ──
    {
        std::println("\n--- AC3: demo-sv-self-evolution reachable ---");
        CompilerService cs;
        // Without Property/Coverpoint nodes, demo returns #f — still must not hang.
        auto r = cs.eval("(eda:demo-sv-self-evolution \"interface\" 1)");
        CHECK(r.has_value(), "demo returns");
        CHECK(is_bool(*r) || is_int(*r), "demo returns bool or int");
    }

    std::println("\n=== test_demo_sv_nested_guard_1774: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

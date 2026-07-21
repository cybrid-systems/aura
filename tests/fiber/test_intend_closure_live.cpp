// @category: unit
// @reason: Issue #1719 — intend call_fn must not apply_closure on freed
// Issue #1719 (#1978 renamed): issue# moved from filename to header.
// generator/verifier/fixer ClosureIds (UAF sibling of #1713).
//
//   AC1: source call_fn uses agent_cid_live / Issue #1719
//   AC2: agent_closure_freed_during_call metric exists
//   AC3: free generator then intend does not crash; metric bumps
//   AC4: live intend still can complete (or empty without LLM)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC2: source + metric field ──
    {
        std::println("\n--- AC1/AC2: call_fn live gate + metric ---");
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
            CHECK(src.find("Issue #1719") != std::string::npos, "cites #1719");
            CHECK(src.find("agent_cid_live") != std::string::npos, "agent_cid_live shared");
            CHECK(src.find("agent_note_closure_freed_call") != std::string::npos,
                  "call metric helper");
            auto pos = src.find("add(\"intend\"");
            CHECK(pos != std::string::npos, "found intend");
            if (pos != std::string::npos) {
                auto win = src.substr(pos, 6000);
                CHECK(win.find("agent_cid_live") != std::string::npos, "intend gates live");
                CHECK(win.find("agent_closure_freed_during_call") != std::string::npos ||
                          win.find("agent_note_closure_freed_call") != std::string::npos,
                      "intend bumps call metric");
            }
        }
        const char* mpaths[] = {
            "src/compiler/observability_metrics.h",
            "../src/compiler/observability_metrics.h",
        };
        std::string msrc;
        for (const char* p : mpaths) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() && msrc.find("agent_closure_freed_during_call") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: free generator before intend ──
    {
        std::println("\n--- AC3: free generator → no crash, metric bump ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_closure_freed_during_call.load(std::memory_order_relaxed);

        // generator returns empty string; verifier never reached if gen freed.
        auto r = cs.eval(
            R"AURA((begin
                 (define gen (lambda (g) "(define (f x) x)"))
                 (define ver (lambda (c) "#t"))
                 (closure:free! gen)
                 (intend "goal" gen ver)))AURA");
        CHECK(r.has_value(), "intend after free evaluates (no crash)");

        const auto m1 = m->agent_closure_freed_during_call.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "agent_closure_freed_during_call bumped");
    }

    // ── AC4: live path still works ──
    {
        std::println("\n--- AC4: live intend does not crash ---");
        CompilerService cs;
        auto r = cs.eval(
            R"AURA((begin
                 (define gen (lambda (g) "(define (f x) x)"))
                 (define ver (lambda (c) "#t"))
                 (intend "goal" gen ver 1)))AURA");
        CHECK(r.has_value(), "live intend completes");
    }

    std::println("\n=== test_intend_closure_live_1719: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

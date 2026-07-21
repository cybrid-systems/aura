// @category: unit
// @reason: Issue #1713 — auto-evolve-tick/once must not apply_closure on
// Issue #1713 (#1978 renamed): issue# moved from filename to header.
// freed detect/fix ClosureIds (UAF / wrong-fn after free).
//
//   AC1: tick stops + returns #f when detect/fix TW-erased after loop start
//   AC2: agent_closure_freed_during_tick bumps on freed tick
//   AC3: once returns 0 without apply when detect already erased
//   AC4: live detect+fix still run (once returns fixes)
//   AC5: source cites #1713 + find_active_closure / is_freed dual gate

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
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
using aura::compiler::types::as_bool;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
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
    // ── AC4: live once still works ──
    {
        std::println("\n--- AC4: once with live closures ---");
        CompilerService cs;
        // Named defines materialize TW closures reliably for once.
        auto r = cs.eval(
            R"((begin
                 (define d (lambda () (list (list "g"))))
                 (define f (lambda (g) #t))
                 (auto-evolve-once d f)))");
        CHECK(r.has_value(), "once eval ok");
        if (r && is_int(*r))
            CHECK(as_int(*r) >= 1, "once fixed ≥1 with live fns");
        else
            CHECK(false, "once returned int");
    }

    // ── AC1/AC2: free detect after loop start → tick stops ──
    {
        std::println("\n--- AC1/AC2: free detect after loop → tick #f + metric ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_closure_freed_during_tick.load(std::memory_order_relaxed);

        // Bind detect/fix, start loop, free detect, tick.
        auto setup = cs.eval(
            R"((begin
                 (define detect (lambda () '()))
                 (define fix (lambda (g) #f))
                 (auto-evolve-loop "1.0" detect fix)
                 (auto-evolve-running?)))");
        CHECK(setup.has_value(), "loop setup ok");
        if (setup && is_bool(*setup))
            CHECK(as_bool(*setup), "running after loop start");

        auto free_r = cs.eval("(closure:free! detect)");
        CHECK(free_r.has_value(), "closure:free! ok");

        auto tick = cs.eval("(auto-evolve-tick)");
        CHECK(tick.has_value(), "tick eval ok");
        if (tick && is_bool(*tick))
            CHECK(!as_bool(*tick), "tick returns #f after free");
        else
            CHECK(false, "tick returned bool");

        auto running = cs.eval("(auto-evolve-running?)");
        CHECK(running.has_value() && is_bool(*running) && !as_bool(*running),
              "loop stopped after freed detect");

        const auto m1 = m->agent_closure_freed_during_tick.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "agent_closure_freed_during_tick bumped");
    }

    // ── AC3: once with erased detect ──
    {
        std::println("\n--- AC3: once after free detect returns 0 ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_closure_freed_during_tick.load(std::memory_order_relaxed);

        auto r = cs.eval(
            R"((begin
                 (define d (lambda () (list (list "x"))))
                 (define f (lambda (g) #t))
                 (closure:free! d)
                 (auto-evolve-once d f)))");
        CHECK(r.has_value(), "once-after-free eval ok");
        if (r && is_int(*r))
            CHECK(as_int(*r) == 0, "once returns 0 on freed detect");
        else
            CHECK(false, "once returned int");

        const auto m1 = m->agent_closure_freed_during_tick.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "metric bumped on once freed");
    }

    // ── AC5: source audit ──
    {
        std::println("\n--- AC5: source dual gate ---");
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
            CHECK(src.find("Issue #1713") != std::string::npos, "cites #1713");
            CHECK(src.find("find_active_closure") != std::string::npos, "TW live check");
            CHECK(src.find("aura_closure_is_freed") != std::string::npos, "JIT free check");
            CHECK(src.find("agent_closure_freed_during_tick") != std::string::npos, "metric bump");
            auto pos = src.find("add(\"auto-evolve-tick\"");
            CHECK(pos != std::string::npos, "found tick");
            if (pos != std::string::npos) {
                auto win = src.substr(pos, 1800);
                CHECK(win.find("agent_cid_live") != std::string::npos, "tick gates on live check");
            }
        }
    }

    std::println("\n=== test_auto_evolve_closure_live_1713: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

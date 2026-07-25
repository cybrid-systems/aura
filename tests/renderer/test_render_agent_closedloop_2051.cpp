// @category: unit
// @reason: Issue #2051 — Agent-visible render stats + closed-loop
// mutate-optimize for self-evolving terminal engines.
//
//   AC1: query:render-stats schema-2051 + Agent decision keys present
//   AC2: controlled present/mutate workload moves counters (live match)
//   AC3: render-critical soft-dirty advances render-mutate-cost samples
//   AC4: closed loop: read → mutate strategy → present → re-read (no regress)
//   AC5: adaptive path driven only by query surface (agent-action)
//   AC6: closed-loop tick stamps rounds/stable/improve; evolution stats sibling
//   AC7: template contract stamps (safe window / issue)

#include "test_harness.hpp"

#include "compiler/render_prim_template.hh"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_rs(CompilerService& cs, std::string_view key) {
    return href(cs, "query:render-stats", key);
}

} // namespace

int main() {
    // ── AC1: schema-2051 Agent surface ──
    {
        std::println("--- AC1: schema-2051 keys ---");
        CompilerService cs;
        auto h = cs.eval("(stats:get \"query:render-stats\")");
        CHECK(h && is_hash(*h), "query:render-stats hash");
        CHECK(href_rs(cs, "schema") >= 1674, "base schema lineage");
        CHECK(href_rs(cs, "schema-2051") == 2051, "schema-2051");
        CHECK(href_rs(cs, "issue-2051") == 2051, "issue-2051");
        CHECK(href_rs(cs, "agent-closedloop-wired") == 1, "agent-closedloop-wired");
        CHECK(href_rs(cs, "render-critical-protect-wired") == 1, "protect wired");
        CHECK(href_rs(cs, "throttle-window-ms") == 500, "throttle window 500ms");
        for (const char* k :
             {"frame-time-avg-us", "frame-time-p99-us", "dirty-short-circuit-rate-bp",
              "deopt-throttled", "deopt-applied", "render-critical-deopt-throttled",
              "render-critical-jit-keep", "render-mutate-cost-samples", "render-mutate-avg-us",
              "render-arena-pressure-bp", "agent-health-score", "safe-to-mutate", "agent-action",
              "closed-loop-rounds", "sibling-memory-schema", "sibling-jit-stability-schema"}) {
            CHECK(href_rs(cs, k) >= 0, std::format("{} present", k));
        }
        const auto health = href_rs(cs, "agent-health-score");
        CHECK(health >= 0 && health <= 100, "health in [0,100]");
        const auto action = href_rs(cs, "agent-action");
        CHECK(action >= 0 && action <= 4, "agent-action 0..4");
        CHECK(href_rs(cs, "sibling-memory-schema") == 2049, "sibling memory 2049");
        CHECK(href_rs(cs, "sibling-jit-stability-schema") == 2050, "sibling jit 2050");
    }

    // ── AC2: present workload moves counters ──
    {
        std::println("--- AC2: present workload ---");
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 12 6)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);
        const auto p0 = href_rs(cs, "present-batch-total");
        const auto hits0 = href_rs(cs, "obs-v2-hits");
        for (int i = 0; i < 20; ++i) {
            (void)cs.eval(std::format("(terminal-set-cell {} {} {} {} {} 0)", bid, i % 12, i % 6,
                                      65 + (i % 26), (i % 7) + 1));
            auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
            CHECK(p && is_int(*p) && as_int(*p) >= 0, "present");
        }
        (void)cs.eval("(stats:get \"query:render-stats\")");
        const auto p1 = href_rs(cs, "present-batch-total");
        CHECK(p1 > p0, std::format("present-batch-total {} > {}", p1, p0));
        CHECK(href_rs(cs, "set-cell-total") >= 20, "set-cell-total advanced");
        CHECK(href_rs(cs, "agent-health-score") >= 50, "health still healthy after present");
        // Query path itself is cheap (relaxed); hits may advance via obs-v2.
        (void)hits0;
    }

    // ── AC3: render mutate cost on evolution soft-dirty ──
    {
        std::println("--- AC3: render-mutate-cost ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (draw-frame-agent x) (+ x 1))\")").has_value(),
              "set-code draw-frame-agent");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        const auto samples0 = href_rs(cs, "render-mutate-cost-samples");
        const auto ns0 = href_rs(cs, "render-mutate-cost-ns-total");
        for (int i = 0; i < 8; ++i)
            cs.public_mark_define_dirty("draw-frame-agent");
        const auto samples1 = href_rs(cs, "render-mutate-cost-samples");
        const auto ns1 = href_rs(cs, "render-mutate-cost-ns-total");
        const auto last = href_rs(cs, "render-mutate-last-ns");
        CHECK(samples1 - samples0 >= 8, std::format("cost samples +{}", samples1 - samples0));
        CHECK(ns1 > ns0, "cost ns total advanced");
        CHECK(last >= 0, "last-ns present");
        CHECK(href_rs(cs, "render-mutate-avg-us") >= 0, "avg-us present");
        // Evolution sibling also exposes cost samples
        CHECK(href(cs, "query:render-evolution-stats", "render-mutate-cost-samples") >= samples1,
              "evolution-stats cost samples");
        CHECK(href(cs, "query:render-evolution-stats", "schema-2051") == 2051,
              "evolution schema-2051");
    }

    // ── AC4+AC5: closed loop driven by query surface ──
    {
        std::println("--- AC4/AC5: Agent closed loop ---");
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 16 8)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "buf");
        const auto bid = as_int(*id);
        CHECK(cs.eval("(set-code \"(define (present-style n) (+ n 0))\")").has_value(),
              "set-code present-style");
        CHECK(cs.eval("(eval-current)").has_value(), "eval present-style");

        // Warm presents
        for (int w = 0; w < 8; ++w) {
            (void)cs.eval(std::format("(terminal-set-cell {} {} {} 65 1 0)", bid, w % 16, w % 8));
            (void)cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        }

        const auto health0 = href_rs(cs, "agent-health-score");
        const auto present0 = href_rs(cs, "present-batch-total");
        const auto thr0 = href_rs(cs, "render-critical-deopt-throttled");
        const auto keep0 = href_rs(cs, "render-critical-jit-keep");
        CHECK(health0 >= 50, "baseline health");

        constexpr int kRounds = 24;
        for (int r = 0; r < kRounds; ++r) {
            // 1. Observe
            const auto health = href_rs(cs, "agent-health-score");
            const auto safe = href_rs(cs, "safe-to-mutate");
            const auto action = href_rs(cs, "agent-action");
            CHECK(health >= 0 && health <= 100, "health each round");
            CHECK(action >= 0 && action <= 4, "action each round");

            // 2. Decide from query surface only
            if (action == 4) {
                // stop-mutate: only present
            } else if (action == 2) {
                // reduce frequency: mutate every 4th round
                if (r % 4 == 0 && safe == 1)
                    cs.public_mark_define_dirty("present-style");
            } else if (action == 3) {
                // prefer dirty-delta via render-optimize
                (void)cs.eval(std::format("(mutate :render-optimize {})", bid));
            } else if (action == 1 || safe == 1) {
                // optimize-ok / hold-with-safe: light set-body every 3rd
                if (r % 3 == 0) {
                    (void)cs.eval(std::format(
                        "(mutate:set-body \"present-style\" \"(lambda (n) (+ n {}))\" \"r{}\")",
                        r % 5, r));
                }
            }

            // 3. Present
            (void)cs.eval(std::format("(terminal-set-cell {} {} {} {} {} 0)", bid, r % 16, r % 8,
                                      66 + (r % 20), (r % 7) + 1));
            auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
            CHECK(p && is_int(*p) && as_int(*p) >= 0, "present in loop");

            // 4. Re-read + stamp outcome via (mutate :closed-loop-tick …)
            const auto health1 = href_rs(cs, "agent-health-score");
            if (health1 >= health)
                (void)cs.eval("(mutate :closed-loop-tick 2)"); // improve
            else if (health1 + 15 >= health)
                (void)cs.eval("(mutate :closed-loop-tick 1)"); // stable
            else
                (void)cs.eval("(mutate :closed-loop-tick)"); // round only
        }

        const auto health1 = href_rs(cs, "agent-health-score");
        const auto present1 = href_rs(cs, "present-batch-total");
        const auto thr1 = href_rs(cs, "render-critical-deopt-throttled");
        const auto keep1 = href_rs(cs, "render-critical-jit-keep");
        const auto rounds = href_rs(cs, "closed-loop-rounds");
        const auto stable = href_rs(cs, "closed-loop-stable");
        const auto improve = href_rs(cs, "closed-loop-improve");

        CHECK(present1 > present0, "presents advanced in closed loop");
        // No hard regression of Agent health under adaptive strategy
        CHECK(health1 + 25 >= health0,
              std::format("health no hard regress {} → {}", health0, health1));
        CHECK(rounds >= kRounds / 2, std::format("closed-loop rounds {} >= half", rounds));
        CHECK(stable + improve >= 1, "at least one stable/improve stamp");
        // Protection should engage when mutates hit evolution names
        CHECK(thr1 >= thr0 || keep1 >= keep0 || href_rs(cs, "render-mutate-cost-samples") > 0,
              "protect or cost counters advanced under mutate");
        CHECK(href_rs(cs, "safe-to-mutate") == 0 || href_rs(cs, "safe-to-mutate") == 1,
              "safe-to-mutate flag");
    }

    // ── AC6: tick facade + evolution sibling ──
    {
        std::println("--- AC6: closed-loop tick facade ---");
        CompilerService cs;
        const auto r0 = href_rs(cs, "closed-loop-rounds");
        auto t = cs.eval("(mutate :closed-loop-tick)");
        CHECK(t && is_int(*t) && as_int(*t) > r0, "tick advances rounds");
        // Zero-arg facade also works via stats:get (round only).
        (void)cs.eval("(stats:get \"mutate:render-closed-loop-tick\")");
        (void)cs.eval("(mutate :closed-loop-tick 1)");
        (void)cs.eval("(mutate :closed-loop-tick 2)");
        CHECK(href_rs(cs, "closed-loop-stable") >= 1, "stable stamp");
        CHECK(href_rs(cs, "closed-loop-improve") >= 1, "improve stamp");
        CHECK(href(cs, "query:render-evolution-stats", "closed-loop-rounds") >= 1,
              "evolution sees rounds");
        CHECK(href(cs, "query:render-evolution-stats", "safe-mutate-window-ms") ==
                  kRenderSafeMutateWindowMs,
              "safe window on evolution stats");
    }

    // ── AC7: template contract constants ──
    {
        std::println("--- AC7: template contract ---");
        CHECK(kRenderAgentClosedLoopIssue == 2051, "kRenderAgentClosedLoopIssue");
        CHECK(kRenderSafeMutateWindowMs == 500, "kRenderSafeMutateWindowMs");
        CHECK(aura_is_render_evolution_name("draw-frame"), "draw-frame is evolution");
        CHECK(!aura_is_render_evolution_name("plain-math"), "plain-math not evolution");
    }

    std::println("\n#2051 render agent closed-loop: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

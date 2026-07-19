// @category: integration
// @reason: Epic #1449 demotion batch — dashboard facade + Tier-1 siblings
//
// Verifies SlimSurface progress after expanding facade-only intercept
// (query:* health/readiness/slo/score dashboards + all *-stats) and
// hard-removing query:siblings from the public engine registry.

#include "test_harness.hpp"

#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace

int main() {
    std::println("=== Epic #1449 demotion batch — facade dashboards + siblings ===");

    // ── AC1: query:siblings not public ──
    {
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        CHECK(prims.slot_for_name("query:siblings") >= prims.slot_count(),
              "query:siblings not public");
    }

    // ── AC2: dashboards not public; still reachable via stats:get ──
    {
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        static constexpr const char* kDash[] = {
            "query:edsl-readiness",
            "query:code-as-data-production-health",
            "query:runtime-production-health",
            "query:prompt6-safety-score",
            "query:task6-concurrent-fidelity",
            "query:sv-closedloop-slo",
            "query:cxx26-invariants",
        };
        for (const char* name : kDash) {
            CHECK(prims.slot_for_name(name) >= prims.slot_count(),
                  std::format("{} not public add()", name));
            auto via = cs.eval(std::format("(stats:get \"{}\")", name));
            // May be void if not registered this build path; must not crash.
            CHECK(via.has_value(), std::format("stats:get \"{}\" callable", name));
            if (via && !is_void(*via))
                CHECK(is_hash(*via) || is_int(*via),
                      std::format("stats:get \"{}\" hash|int when live", name));
        }
    }

    // ── AC3: all public *-stats remain 0 ──
    {
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        std::size_t stats_public = 0;
        // Sample known former residual names
        static constexpr const char* kStats[] = {
            "gc-stats",          "arena:adaptive-stats", "ast:generation-stats",
            "concurrency:stats", "string-pool:stats",    "atomic-batch:stats",
            "closure:stats",     "dirty:summary",
        };
        for (const char* name : kStats) {
            if (prims.slot_for_name(name) < prims.slot_count())
                ++stats_public;
        }
        CHECK(stats_public == 0,
              std::format("sampled *-stats/dash not public (got {})", stats_public));
    }

    // ── AC3b: dirty:* facade ──
    {
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        CHECK(prims.slot_for_name("dirty:summary") >= prims.slot_count(),
              "dirty:summary not public");
        auto d = cs.eval("(stats:get \"dirty:summary\")");
        CHECK(d.has_value(), "stats:get dirty:summary callable");
    }

    // ── AC4: compat still provides siblings ──
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/compat\" all:)").has_value(), "require compat");
        auto r = cs.eval("(query:siblings 0)");
        CHECK(r.has_value(), "compat query:siblings resolves");
    }

    // ── AC5: docs updated ──
    CHECK(file_exists("docs/design/epic-1449-surface-slim-v2.md"), "epic tracking doc");
    {
        std::ifstream f("docs/design/epic-1449-surface-slim-v2.md");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("1449") != std::string::npos, "epic doc mentions 1449");
        CHECK(content.find("412") != std::string::npos || content.find("core") != std::string::npos,
              "epic doc tracks core budget");
    }

    // ── AC6: batch-3 samples not public ──
    {
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        CHECK(prims.slot_for_name("compile:status") >= prims.slot_count(),
              "compile:status not public");
        CHECK(prims.slot_for_name("mutation-count") >= prims.slot_count(),
              "mutation-count not public");
        CHECK(prims.slot_for_name("ast:generation") >= prims.slot_count(),
              "ast:generation not public");
        // Control GC stays public (must not have been demoted to facade-only).
        CHECK(prims.slot_for_name("gc-heap") < prims.slot_count(),
              "gc-heap remains public control");
        auto g = cs.eval("(gc-heap)");
        CHECK(g.has_value(), "gc-heap still callable as control");
    }

    std::println("\n─── #1449 demotion batch: {}/{} passed, {} failed ───", g_passed,
                 g_passed + g_failed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

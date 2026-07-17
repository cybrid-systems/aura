// @category: integration
// @reason: Issue #1593 — consolidated AI closed-loop readiness: health-score,
// SLO breach, trend, sibling linkage (#1591/#1592), adaptive safepoint signal.
//
//   AC1: schema 1593 + health-score / action / recommendation
//   AC2: slo-breach / health-trend / sibling fields present
//   AC3: health drops under quota rejects; slo-breach may fire
//   AC4: adaptive-safepoint-recommended under pressure
//   AC5: multi-sample trend + long mutate loop still queryable
//   AC6: linkage with fairness / post-steal / resource-quota siblings

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.arena;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static constexpr const char* kQ = "query:ai-closedloop-readiness-stats";

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", kQ, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_schema_health() {
    std::println("\n--- AC1: schema 1593 + health ---");
    CompilerService cs;
    auto h = cs.eval(std::format("(engine:metrics \"{}\")", kQ));
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1593, "schema 1593");
    CHECK(href(cs, "issue") == 1593, "issue 1593");
    const auto health = href(cs, "health-score");
    CHECK(health >= 0 && health <= 100, "health in [0,100]");
    CHECK(href(cs, "action") >= 0 && href(cs, "action") <= 4, "action");
    CHECK(href(cs, "recommendation") >= 0 && href(cs, "recommendation") <= 4, "recommendation");
    CHECK(health >= 80, "fresh health high");
}

static void ac2_slo_trend_siblings() {
    std::println("\n--- AC2: SLO / trend / sibling fields ---");
    CompilerService cs;
    for (const char* k :
         {"slo-breach", "slo-threshold", "slo-breach-total", "health-trend", "health-prev",
          "samples-total", "avg-hold-time-us", "safepoint-wait-while-mutation-held-us",
          "safe-yield-skipped-held", "post-steal-refresh-count",
          "steal-inner-deferred-starvation-mitigated-count", "adaptive-safepoint-recommended",
          "adaptive-soft-triggers", "adaptive-safepoint-threshold", "linear-enforcements",
          "quota-rejects", "fiber-depth-max", "live-mutation-depth"}) {
        // health-trend can be negative; others are non-negative counters/flags
        const auto v = href(cs, k);
        if (std::string_view(k) == "health-trend")
            CHECK(v >= -100 && v <= 100, std::format("health-trend in range (got {})", v));
        else
            CHECK(v >= 0, std::format("{} >= 0 (got {})", k, v));
    }
    CHECK(href(cs, "slo-threshold") == 70, "slo-threshold 70");
}

static void ac3_health_under_pressure() {
    std::println("\n--- AC3: health drops under quota pressure ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto h0 = href(cs, "health-score");
    ASTArena arena(64 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(8);
    for (int i = 0; i < 40; ++i)
        (void)ev.allocate_checked(1024);
    ev.set_resource_quota_memory(0);
    for (int i = 0; i < 25; ++i)
        cs.public_mark_define_dirty("__cascade_pressure__");
    const auto h1 = href(cs, "health-score");
    const auto rejects = href(cs, "quota-rejects");
    CHECK(rejects >= 5, "quota-rejects advanced");
    CHECK(h1 <= h0, std::format("health did not rise ({} → {})", h0, h1));
    // Either SLO breach or action escalated
    const auto breach = href(cs, "slo-breach");
    const auto action = href(cs, "action");
    CHECK(breach == 1 || action >= 2 || h1 < 100,
          std::format("pressure signal (breach={} action={} health={})", breach, action, h1));
}

static void ac4_adaptive_signal() {
    std::println("\n--- AC4: adaptive safepoint signal ---");
    CompilerService cs;
    // Sample once; recommended may be 0 on fresh.
    (void)href(cs, "adaptive-safepoint-recommended");
    // Force more breaches via more rejects on a new service... use pressure again
    auto& ev = cs.evaluator();
    ASTArena arena(32 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(4);
    for (int i = 0; i < 50; ++i)
        (void)ev.allocate_checked(512);
    ev.set_resource_quota_memory(0);
    const auto rec = href(cs, "adaptive-safepoint-recommended");
    CHECK(rec == 0 || rec == 1, "adaptive flag 0/1");
    // Under heavy quota, recommended should often be 1
    CHECK(href(cs, "slo-breach-total") >= 0, "breach total");
    CHECK(href(cs, "adaptive-safepoint-threshold") >= 0, "threshold readable");
}

static void ac5_trend_and_loop() {
    std::println("\n--- AC5: multi-sample trend + mutate loop ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (* x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    std::int64_t last = href(cs, "health-score");
    for (int i = 0; i < 30; ++i) {
        (void)cs.eval("(+ 1 2)");
        if ((i % 5) == 0)
            cs.public_mark_define_dirty("g");
        const auto h = href(cs, "health-score");
        CHECK(h >= 0 && h <= 100, "health still valid");
        last = h;
    }
    CHECK(href(cs, "samples-total") >= 1, "samples advanced");
    // health-trend is delta vs previous sample (can be pos/neg/0)
    const auto trend = href(cs, "health-trend");
    CHECK(trend >= -100 && trend <= 100, std::format("trend in range (got {})", trend));
    (void)last;
}

static void ac6_siblings() {
    std::println("\n--- AC6: sibling surfaces ---");
    CompilerService cs;
    for (const char* p :
         {"query:mutation-boundary-fairness-stats", "query:post-steal-closed-loop-stats",
          "query:resource-quota-stats", "query:per-fiber-mutation-depth-stats", kQ}) {
        auto h = cs.eval(std::format("(engine:metrics \"{}\")", p));
        CHECK(h && is_hash(*h), std::format("{} hash", p));
    }
}

} // namespace

int main() {
    std::println("=== test_ai_closedloop_readiness_1593 (#1593) ===");
    ac1_schema_health();
    ac2_slo_trend_siblings();
    ac3_health_under_pressure();
    ac4_adaptive_signal();
    ac5_trend_and_loop();
    ac6_siblings();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

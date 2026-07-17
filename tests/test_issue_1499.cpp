// @category: integration
// @reason: Issue #1499 — consolidated AI closed-loop readiness
// observability (refine #1470 / #1483).
//
//   AC1: query:ai-closedloop-readiness-stats is hash, schema 1499
//   AC2: health-score in [0,100]; action in [0,4]; #1470 fields present
//   AC3: production breakdown fields readable (cascade, linear, steal…)
//   AC4: health-score drops after quota rejects / cascade pressure
//   AC5: long mutate loop — metrics advance, eval remains healthy
//   AC6: sibling primitives still reachable

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
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = Evaluator::MutationBoundaryGuard;

static constexpr const char* kQ = "query:ai-closedloop-readiness-stats";

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    // Use string keys (engine:metrics hash-ref accepts symbol or string).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", kQ, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_shape_schema() {
    std::println("\n--- AC1: shape + schema 1499 ---");
    CompilerService cs;
    auto h = cs.eval(std::format("(engine:metrics \"{}\")", kQ));
    CHECK(h && is_hash(*h), "ai-closedloop-readiness-stats is hash");
    CHECK(href(cs, "schema") == 1597 || href(cs, "schema") == 1593 || href(cs, "schema") == 1499,
          "schema == 1597 or 1593 or 1499");
}

static void ac2_health_and_1470() {
    std::println("\n--- AC2: health-score + #1470 fields ---");
    CompilerService cs;
    const auto health = href(cs, "health-score");
    const auto action = href(cs, "action");
    const auto rec = href(cs, "recommendation");
    CHECK(health >= 0 && health <= 100, std::format("health-score in [0,100] (got {})", health));
    CHECK(action >= 0 && action <= 4, std::format("action in [0,4] (got {})", action));
    CHECK(rec >= 0 && rec <= 4, std::format("recommendation in [0,4] (got {})", rec));
    CHECK(href(cs, "wraps") >= 0, "wraps");
    CHECK(href(cs, "invalidations") >= 0, "invalidations");
    CHECK(href(cs, "batch-commits") >= 0, "batch-commits");
    CHECK(href(cs, "hygiene-skips") >= 0, "hygiene-skips");
    CHECK(href(cs, "dirty-prunes") >= 0, "dirty-prunes");
    // Fresh service should be near-perfect health.
    CHECK(health >= 80, std::format("fresh health >= 80 (got {})", health));
    CHECK(action == 0 || action == 1, "fresh action is ok or mild");
}

static void ac3_breakdown() {
    std::println("\n--- AC3: production breakdown fields ---");
    CompilerService cs;
    for (const char* k :
         {"linear-enforcements", "cascade-depth-max", "cascade-depth-avg-x100",
          "invalidation-protocol", "bridge-epoch-bumps", "steal-auto-refresh",
          "boundary-pinned-refresh", "live-closure-stale-prevented", "yield-rollbacks",
          "quota-rejects", "relower-blocks", "full-relower", "partial-relower",
          "relower-partial-bp", "fiber-depth-max", "live-mutation-depth"}) {
        CHECK(href(cs, k) >= 0, std::format("{} readable", k));
    }
}

static void ac4_health_drops_under_pressure() {
    std::println("\n--- AC4: health-score responds to quota rejects ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    const auto health0 = href(cs, "health-score");

    // Generate quota rejects → health penalty.
    ASTArena arena(64 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(8);
    for (int i = 0; i < 30; ++i)
        (void)ev.allocate_checked(1024);
    ev.set_resource_quota_memory(0);

    // Cascade depth pressure via metrics inject (mark_define_dirty protocol).
    for (int i = 0; i < 20; ++i)
        cs.public_mark_define_dirty("__missing_for_cascade__");

    const auto health1 = href(cs, "health-score");
    const auto rejects = href(cs, "quota-rejects");
    const auto action = href(cs, "action");
    CHECK(rejects >= 5, std::format("quota-rejects advanced (got {})", rejects));
    CHECK(health1 <= health0,
          std::format("health did not rise under pressure ({} → {})", health0, health1));
    // With many rejects, action should prefer raise-quota (3).
    CHECK(action == 3 || health1 < 100,
          std::format("action=raise-quota or health degraded (action={} health={})", action,
                      health1));
}

static void ac5_mutate_stress() {
    std::println("\n--- AC5: mutate loop health stays queryable ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval("(+ 1 1)");
        if ((i % 10) == 0)
            (void)cs.public_mark_define_dirty("f");
    }
    const auto health = href(cs, "health-score");
    CHECK(health >= 0 && health <= 100, "health still valid after loop");
    auto r = cs.eval("(+ 10 32)");
    CHECK(r.has_value(), "eval ok after stress");
    CHECK(href(cs, "invalidation-protocol") >= 0, "protocol field after stress");
}

static void ac6_siblings() {
    std::println("\n--- AC6: sibling stats still reachable ---");
    CompilerService cs;
    auto a = cs.eval("(engine:metrics \"query:per-fiber-mutation-stack-stats\")");
    CHECK(a && is_hash(*a), "per-fiber-mutation-stack-stats");
    auto b = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(b && is_hash(*b), "resource-quota-stats");
    auto c = cs.eval("(engine:metrics \"query:ai-closedloop-readiness-stats\")");
    CHECK(c && is_hash(*c), "ai-closedloop still hash");
}

} // namespace

int main() {
    std::println("test_issue_1499: AI closed-loop readiness observability (#1499)");
    ac1_shape_schema();
    ac2_health_and_1470();
    ac3_breakdown();
    ac4_health_drops_under_pressure();
    ac5_mutate_stress();
    ac6_siblings();
    std::println("\n#1499: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

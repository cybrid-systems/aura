// @category: integration
// @reason: Issue #1883 — query:self-evolution-loop-stats hash + aot-hotupdate-stats audit fields
//
// AC1: self-evolution-loop-stats is hash schema 1883 with total + rate fields
// AC2: aot-hotupdate-stats includes hotupdate-attempts/success/fail/invariant-fail
// AC3: multi-round mutate loop keeps total monotonic; stack depth fields present
// AC4: per-fiber-mutation-stack-stats remains reachable (compat)

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::capture_aot_hotupdate_audit;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_sample_ratio;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") '{}')", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_self_evolution_hash(CompilerService& cs) {
    std::println("\n--- AC1: self-evolution-loop-stats hash schema 1883 ---");
    auto h = cs.eval("(engine:metrics \"query:self-evolution-loop-stats\")");
    CHECK(h && is_hash(*h), "self-evolution-loop-stats is hash");
    CHECK(href(cs, "query:self-evolution-loop-stats", "schema") == 1883, "schema 1883");
    CHECK(href(cs, "query:self-evolution-loop-stats", "active") == 1, "active");
    CHECK(href(cs, "query:self-evolution-loop-stats", "total") >= 0, "total >= 0");
    CHECK(href(cs, "query:self-evolution-loop-stats", "mutation-success-rate-bp") >= 0,
          "mutation-success-rate-bp");
    CHECK(href(cs, "query:self-evolution-loop-stats", "invariant-pass-rate-bp") >= 0,
          "invariant-pass-rate-bp");
    CHECK(href(cs, "query:self-evolution-loop-stats", "stack-depth-lifetime-max") >= 0,
          "stack-depth-lifetime-max");
    CHECK(href(cs, "query:self-evolution-loop-stats", "stack-depth-live") >= 0, "stack-depth-live");
}

void ac2_aot_hotupdate_fields(CompilerService& cs) {
    std::println("\n--- AC2: aot-hotupdate-stats hotupdate-* fields ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_sample_ratio(1);
    capture_aot_hotupdate_audit(true, 1, 2, "aot-hotupdate-synth");
    capture_aot_hotupdate_audit(false, 2, 3, "aot-hotupdate-synth-fail");

    auto h = cs.eval("(engine:metrics \"query:aot-hotupdate-stats\")");
    CHECK(h && is_hash(*h), "aot-hotupdate-stats is hash");
    CHECK(href(cs, "query:aot-hotupdate-stats", "schema") == 590, "schema still 590");
    CHECK(href(cs, "query:aot-hotupdate-stats", "issue-1883-extended") == 1, "1883 extended");
    CHECK(href(cs, "query:aot-hotupdate-stats", "hotupdate-attempts") >= 2, "attempts >= 2");
    CHECK(href(cs, "query:aot-hotupdate-stats", "hotupdate-success") >= 1, "success >= 1");
    CHECK(href(cs, "query:aot-hotupdate-stats", "hotupdate-fail") >= 1, "fail >= 1");
    CHECK(href(cs, "query:aot-hotupdate-stats", "hotupdate-invariant-fail") >= 1,
          "invariant-fail >= 1");
    // isolation fields still present
    CHECK(href(cs, "query:aot-hotupdate-stats", "region-isolation") >= 0, "region-isolation");
}

void ac3_loop_monotonic(CompilerService& cs) {
    std::println("\n--- AC3: multi-round mutate, total monotonic ---");
    const auto t0 = href(cs, "query:self-evolution-loop-stats", "total");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval(std::format("(mutate:rebind \"x\" \"{}\")", 10 + i));
        (void)cs.eval("(eval-current)");
    }
    const auto t1 = href(cs, "query:self-evolution-loop-stats", "total");
    CHECK(t1 >= t0, "total monotonic after mutate loop");
    CHECK(href(cs, "query:self-evolution-loop-stats", "mutation-total") >= 0, "mutation-total");
    CHECK(href(cs, "query:self-evolution-loop-stats", "aot-hotupdate-attempts") >= 0,
          "aot attempts surfaced");
}

void ac4_stack_stats_compat(CompilerService& cs) {
    std::println("\n--- AC4: per-fiber-mutation-stack-stats still reachable ---");
    auto h = cs.eval("(engine:metrics \"query:per-fiber-mutation-stack-stats\")");
    CHECK(h && is_hash(*h), "per-fiber-mutation-stack-stats hash");
    CHECK(href(cs, "query:per-fiber-mutation-stack-stats", "lifetime-max") >= 0, "lifetime-max");
}

} // namespace

int main() {
    std::println("=== Issue #1883: self-evolution-loop-stats + aot-hotupdate aggregation ===");
    CompilerService cs;
    ac1_self_evolution_hash(cs);
    ac2_aot_hotupdate_fields(cs);
    ac3_loop_monotonic(cs);
    ac4_stack_stats_compat(cs);
    std::println("\n=== #1883: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

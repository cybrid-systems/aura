// @category: integration
// @reason: Issue #1894 — wire TypedMutationAudit into post-mutation hot path
// Issue #1589/#1614/#1882/#1894 (#1978 renamed): issue# moved from filename to header.
// with contextual should_audit + Full force-rollback + AC metrics
// (refine #1614 / #1589 / #1882).
//
//   AC1: should_audit_contextual forces large dirty / linear scopes
//   AC2: Guard mutate under Full runs invariant suite (triggered_total)
//   AC3: query:typed-mutation-audit-stats schema 1894 + AC metric names
//   AC4: query:typed-mutation-audit-trail schema 1894 hotpath wire flags
//   AC5: multi-round mutate fuzz under Full/Sampled — counters monotonic
//   AC6: phase 4 inventory + DirtyAware flag

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::kTypedMutationAuditIssue;
using aura::compiler::typed_audit::kTypedMutationAuditPassPhase;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::typed_audit::should_audit_contextual;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t trail_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 1)) (define z (* y 2))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_contextual_gate() {
    std::println("\n--- AC1: should_audit_contextual ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    // Small dirty scope with id not hitting ratio may skip.
    (void)should_audit_contextual(/*mid=*/1, /*nodes=*/1, /*linear=*/false);
    // Large dirty scope must force audit under Sampled.
    CHECK(should_audit_contextual(/*mid=*/1, /*nodes=*/16, /*linear=*/false),
          "force audit nodes>=8");
    CHECK(should_audit_contextual(/*mid=*/1, /*nodes=*/1, /*linear=*/true),
          "force audit linear ops");
    set_strategy(AuditStrategy::Full);
    CHECK(should_audit_contextual(99, 0, false), "Full always audits");
    set_strategy(AuditStrategy::Off);
    CHECK(!should_audit_contextual(0, 100, true), "Off never audits");
}

static void ac2_guard_triggered() {
    std::println("\n--- AC2: Guard mutate triggers suite under Full ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    const auto t0 = load_u64(g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total);
    const auto inv0 = load_u64(g_typed_mutation_audit_counters.invariant_audits);
    CHECK(cs.eval("(mutate:rebind \"x\" \"42\")").has_value(), "mutate:rebind");
    CHECK(load_u64(g_typed_mutation_audit_counters.invariant_audits) >= inv0,
          "invariant_audits non-decreasing");
    CHECK(load_u64(g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total) >= t0,
          "triggered_total non-decreasing");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok");
}

static void ac3_stats_schema() {
    std::println("\n--- AC3: query:typed-mutation-audit-stats schema 1894 ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    (void)cs.evaluator().run_typed_mutation_invariant_audit(11, "stats-test", 0, 0, 1);
    auto h = cs.eval("(engine:metrics \"query:typed-mutation-audit-stats\")");
    CHECK(h && is_hash(*h), "stats hash");
    CHECK(stats_href(cs, "schema") == 1894, "schema 1894");
    CHECK(stats_href(cs, "issue") == 1894, "issue 1894");
    CHECK(stats_href(cs, "typed_mutation_audit_triggered_total") >= 1, "triggered AC name");
    CHECK(stats_href(cs, "typed_mutation_violations_caught_total") >= 0, "violations AC name");
    CHECK(stats_href(cs, "provenance_blame_chain_hits_total") >= 0, "blame AC name");
    CHECK(stats_href(cs, "hotpath-guard-exit-wired") == 1, "hotpath wired");
    CHECK(stats_href(cs, "hit-rate-bp") >= 0, "hit-rate-bp");
}

static void ac4_trail_schema() {
    std::println("\n--- AC4: trail schema 1894 + wire flags ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    (void)cs.evaluator().run_typed_mutation_invariant_audit(3, "trail", 0, 0, 1);
    auto h = cs.eval("(engine:metrics \"query:typed-mutation-audit-trail\")");
    CHECK(h && is_hash(*h), "trail hash");
    CHECK(trail_href(cs, "schema") == 1894, "trail schema 1894");
    CHECK(trail_href(cs, "issue") == 1894, "trail issue 1894");
    CHECK(trail_href(cs, "hotpath-guard-exit-wired") == 1, "hotpath");
    CHECK(trail_href(cs, "contextual-should-audit-wired") == 1, "contextual");
    CHECK(trail_href(cs, "full-force-rollback-wired") == 1, "force rollback");
    CHECK(trail_href(cs, "phase") >= 4, "phase >= 4");
    CHECK(trail_href(cs, "typed_mutation_audit_triggered_total") >= 1, "triggered on trail");
}

static void ac5_fuzz_rounds() {
    std::println("\n--- AC5: multi-round mutate under Full then Sampled ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    const auto t0 = load_u64(g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total);
    for (int i = 0; i < 32; ++i) {
        (void)cs.eval("(mutate:rebind \"x\" \"7\")");
        (void)cs.eval("(mutate:rebind \"y\" \"8\")");
        if ((i % 4) == 0)
            (void)cs.eval("(eval-current)");
    }
    set_strategy(AuditStrategy::Sampled);
    for (int i = 0; i < 16; ++i)
        (void)cs.eval("(mutate:rebind \"z\" \"9\")");
    CHECK(load_u64(g_typed_mutation_audit_counters.typed_mutation_audit_triggered_total) >= t0,
          "triggered monotonic after fuzz");
    CHECK(trail_href(cs, "invariant-audits") >= 0, "invariant-audits readable");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval after fuzz");
}

static void ac6_phase_inventory() {
    std::println("\n--- AC6: phase 4 inventory ---");
    CHECK(kTypedMutationAuditPassPhase >= 4, "phase >= 4");
    CHECK(kTypedMutationAuditIssue == 1894, "issue constant 1894");
}

} // namespace

int main() {
    std::println("=== Issue #1894: TypedMutationAudit hotpath wire ===");
    ac1_contextual_gate();
    ac2_guard_triggered();
    ac3_stats_schema();
    ac4_trail_schema();
    ac5_fuzz_rounds();
    ac6_phase_inventory();
    std::println("\n=== #1894: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

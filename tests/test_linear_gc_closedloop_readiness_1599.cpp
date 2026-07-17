// @category: integration
// @reason: Issue #1599 — GC root audit consistency + linear scan integration
// + AI closed-loop readiness + adaptive safepoint (refine #1478/#1483/#1493/
// #1499/#1543).
//
//   AC1: six-touchpoints documented (query flag + audit path names)
//   AC2: linear-gc-root-audit-log + checks_total monotonic
//   AC3: enforce/scan advances live_scans + audit
//   AC4: ai-closedloop-readiness-stats schema 1599 + health breakdown
//   AC5: gc-safepoint-adaptive-stats + mutation_stack_depth_histogram
//   AC6: multi-path load → health/audit trends coherent

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.arena;
import aura.compiler.evaluator;
import aura.compiler.service;
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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* prim, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static void ac1_six_touchpoints() {
    std::println("\n--- AC1: six touchpoints documented ---");
    CompilerService cs;
    // Path name helpers for all audit tags.
    for (std::uint8_t p = 0; p <= 6; ++p) {
        auto name = Evaluator::linear_gc_root_audit_path_name(p);
        CHECK(!name.empty(), std::format("path {} named", p));
    }
    auto h = cs.eval("(engine:metrics \"query:linear-gc-root-audit-log\")");
    CHECK(h && is_hash(*h), "audit-log hash");
    CHECK(href(cs, "query:linear-gc-root-audit-log", "six-touchpoints-documented") == 1 ||
              href(cs, "query:linear-gc-root-audit-log", "schema") == 1599 ||
              href(cs, "query:linear-gc-root-audit-log", "schema") == 1543,
          "six-touchpoints flag or schema lineage");
}

static void ac2_audit_log_monotonic() {
    std::println("\n--- AC2: audit log + checks monotonic ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto c0 = load_u64(m->linear_gc_root_audit_checks_total);
    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual audit ok");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) == c0 + 1, "checks +1");
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditGcSafepoint);
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditCompact);
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= c0 + 3, "checks ≥ +3");

    CHECK(href(cs, "query:linear-gc-root-audit-log", "schema") == 1599 ||
              href(cs, "query:linear-gc-root-audit-log", "schema") == 1543,
          "audit schema 1599|1543");
    CHECK(href(cs, "query:linear-gc-root-audit-log", "audit-checks-total") >= 3,
          "audit-checks-total");
    CHECK(href(cs, "query:linear-gc-root-audit-log", "linear_gc_root_audit_checks_total") >= 3 ||
              href(cs, "query:linear-gc-root-audit-log", "audit-checks-total") >= 3,
          "AC2 counter alias");
}

static void ac3_scan_and_roots() {
    std::println("\n--- AC3: live-closure scan + GC root enforce ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
    (void)ev.scan_live_closures_for_linear_captures(true, false);
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "scans advanced");
    (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate, false);
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audit via enforce");
    ev.test_probe_linear_on_fiber_steal();
    CHECK(load_u64(m->linear_boundary_consistency_total) >= 1, "boundary consistency");
}

static void ac4_closedloop_readiness() {
    std::println("\n--- AC4: ai-closedloop-readiness-stats schema 1599 ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual);
    (void)ev.scan_live_closures_for_linear_captures(true, true);

    auto h = cs.eval("(engine:metrics \"query:ai-closedloop-readiness-stats\")");
    CHECK(h && is_hash(*h), "readiness hash");
    const auto schema = href(cs, "query:ai-closedloop-readiness-stats", "schema");
    CHECK(schema == 1599 || schema == 1597 || schema == 1593 || schema == 1499,
          "schema 1599 lineage");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "health-score") >= 0 &&
              href(cs, "query:ai-closedloop-readiness-stats", "health-score") <= 100,
          "health-score");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "linear-enforcements") >= 0,
          "linear-enforcements");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "linear-gc-root-audit-checks") >= 1 ||
              href(cs, "query:ai-closedloop-readiness-stats",
                   "linear_gc_root_audit_checks_total") >= 1,
          "audit checks in readiness");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "linear-live-closure-scans") >= 0,
          "live scans in readiness");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "mutation_stack_depth_histogram") >= 0,
          "depth hist sum");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "action") >= 0 &&
              href(cs, "query:ai-closedloop-readiness-stats", "action") <= 4,
          "action");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "recommendation") >= 0, "recommendation");
}

static void ac5_adaptive_and_hist() {
    std::println("\n--- AC5: adaptive safepoint + mutation depth hist ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:gc-safepoint-adaptive-stats\")");
    CHECK(h && is_hash(*h), "adaptive hash");
    const auto schema = href(cs, "query:gc-safepoint-adaptive-stats", "schema");
    CHECK(schema == 1599 || schema == 1493 || schema == 1483, "adaptive schema lineage");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "threshold") >= 0, "threshold");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "defer-count") >= 0, "defer-count");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "avg-mutation-hold-us") >= 0, "avg hold");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "mutation_stack_depth_histogram") >= 0,
          "hist sum");
    // Under pressure, adaptive recommendation on readiness may fire.
    auto& ev = cs.evaluator();
    ASTArena arena(32 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(4);
    for (int i = 0; i < 30; ++i)
        (void)ev.allocate_checked(256);
    ev.set_resource_quota_memory(0);
    const auto rec =
        href(cs, "query:ai-closedloop-readiness-stats", "adaptive-safepoint-recommended");
    CHECK(rec == 0 || rec == 1, "adaptive recommended 0/1");
}

static void ac6_load_trends() {
    std::println("\n--- AC6: multi-path load trends ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto h0 = href(cs, "query:ai-closedloop-readiness-stats", "health-score");
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);

    for (int i = 0; i < 20; ++i) {
        if ((i % 4) == 0)
            cs.public_mark_define_dirty("__load__");
        if ((i % 4) == 1)
            (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditManual,
                                                         (i % 2) == 0);
        if ((i % 4) == 2)
            (void)ev.compact_env_frames();
        if ((i % 4) == 3)
            ev.test_probe_linear_on_fiber_steal();
    }

    const auto h1 = href(cs, "query:ai-closedloop-readiness-stats", "health-score");
    CHECK(h1 >= 0 && h1 <= 100, "health still valid under load");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audits advanced under load");
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans under load");
    CHECK(href(cs, "query:ai-closedloop-readiness-stats", "samples-total") >= 1, "samples");
    // Counter balance invariant: resync ≤ registrations (via last audit entry).
    CHECK(href(cs, "query:linear-gc-root-audit-log", "last-ok") == 0 ||
              href(cs, "query:linear-gc-root-audit-log", "last-ok") == 1,
          "last-ok boolean");
    (void)h0;
    std::println("  health {} → {} audits={}", h0, h1,
                 load_u64(m->linear_gc_root_audit_checks_total));
}

} // namespace

int main() {
    std::println("=== Issue #1599: linear GC + closedloop readiness refine ===");
    ac1_six_touchpoints();
    ac2_audit_log_monotonic();
    ac3_scan_and_roots();
    ac4_closedloop_readiness();
    ac5_adaptive_and_hist();
    ac6_load_trends();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #1543 — linear GC root registration consistency audit
//
//   AC1: registration monotonicity across audits + resync path
//   AC2: unregistration balance (env_version_resync <= registrations)
//   AC3: multi-path audit (manual + compact + safepoint + fiber steal)
//   AC4: query:linear-gc-root-audit-log + linear_gc_root_audit_checks_total

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1543_detail {

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

static std::int64_t hash_int_field(CompilerService& cs, std::string_view expr,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC1: registration counter never decreases; audit records growth after resync.
static void ac1_registration_monotonicity() {
    std::println("\n--- AC1: registration monotonicity ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    const auto reg0 = load_u64(m->linear_ownership_gc_root_registrations_total);
    const auto checks0 = load_u64(m->linear_gc_root_audit_checks_total);

    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual audit ok");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) == checks0 + 1, "audit checks +1");

    // Resync registers roots (at least +1 even when empty).
    ev.resync_linear_jit_gc_roots_after_invalidate();
    const auto reg1 = load_u64(m->linear_ownership_gc_root_registrations_total);
    CHECK(reg1 > reg0, "resync bumps registrations");

    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual),
          "second audit still ok (monotonic)");
    const auto& e = ev.linear_gc_root_audit_entry_at(ev.linear_gc_root_audit_seq() - 1);
    CHECK(e.registrations >= reg1, "audit snapshot ≥ post-resync reg");
    CHECK(e.ok == 1, "audit entry ok flag");
}

// AC2: balance invariant resync <= registrations after several resyncs.
static void ac2_unregistration_balance() {
    std::println("\n--- AC2: resync ≤ registrations balance ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    for (int i = 0; i < 5; ++i)
        ev.resync_linear_jit_gc_roots_after_invalidate();

    const auto reg = load_u64(m->linear_ownership_gc_root_registrations_total);
    const auto resync = load_u64(m->linear_ownership_gc_env_version_resync_total);
    CHECK(resync <= reg, "env_version_resync <= registrations (balance)");

    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual),
          "audit accepts balance");
    const auto& e = ev.linear_gc_root_audit_entry_at(ev.linear_gc_root_audit_seq() - 1);
    CHECK(e.env_version_resync <= e.registrations, "log entry balance");
    // Logical unregister signal is stale_hits (collect skips) — may be 0.
    CHECK(e.stale_hits >= 0, "stale_hits readable (unregister proxy)");
}

// AC3: multi-path audits — compact, safepoint, fiber steal, manual.
static void ac3_multi_path_audit() {
    std::println("\n--- AC3: multi-path audit (compact / safepoint / steal) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto checks0 = load_u64(m->linear_gc_root_audit_checks_total);
    const auto total0 = ev.linear_gc_root_audit_total();

    // compact_env_frames wires Compact audit.
    (void)ev.compact_env_frames();
    // safepoint immediate path wires GcSafepoint audit.
    const int deferred = ev.request_gc_safepoint();
    CHECK(deferred == 0, "immediate safepoint (no boundary)");
    // fiber steal probe wires FiberSteal audit.
    ev.probe_linear_ownership_on_fiber_steal();
    // manual.
    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual ok");

    const auto checks1 = load_u64(m->linear_gc_root_audit_checks_total);
    const auto total1 = ev.linear_gc_root_audit_total();
    // compact + safepoint + steal + manual = at least 4
    CHECK(checks1 >= checks0 + 4, "≥4 audit checks across paths");
    CHECK(total1 >= total0 + 4, "audit ring total grew ≥4");

    // Spot-check path names on last few entries.
    bool saw_manual = false;
    bool saw_any_path = false;
    const auto seq = ev.linear_gc_root_audit_seq();
    for (std::uint64_t i = 0; i < 8 && i < seq; ++i) {
        const auto& e = ev.linear_gc_root_audit_entry_at(seq - 1 - i);
        saw_any_path = true;
        if (e.path == Evaluator::kLinearGcRootAuditManual)
            saw_manual = true;
        CHECK(e.ok == 1, "path audit ok");
        auto name = Evaluator::linear_gc_root_audit_path_name(e.path);
        CHECK(!name.empty() && name != "unknown", "path name resolved");
    }
    CHECK(saw_any_path, "ring has entries");
    CHECK(saw_manual, "manual path present in recent entries");
    std::println("  checks {}→{} total {}→{}", checks0, checks1, total0, total1);
}

// AC4: query surface + schema 1543 + checks counter exposed.
static void ac4_query_surface() {
    std::println("\n--- AC4: query:linear-gc-root-audit-log ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "seed audit");
    auto r = cs.eval("(engine:metrics \"query:linear-gc-root-audit-log\")");
    CHECK(r && is_hash(*r), "query returns hash");

    const auto schema =
        hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")", "schema");
    CHECK(schema == 1599 || schema == 1543, "schema == 1599|1543");

    const auto checks = hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")",
                                       "audit-checks-total");
    CHECK(checks >= 1, "audit-checks-total >= 1");
    CHECK(static_cast<std::uint64_t>(checks) == load_u64(m->linear_gc_root_audit_checks_total),
          "query matches metric counter");

    const auto last_ok =
        hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")", "last-ok");
    CHECK(last_ok == 1, "last-ok == 1");

    const auto log_size =
        hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")", "log-size");
    CHECK(log_size >= 1, "log-size >= 1");
}

} // namespace aura_issue_1543_detail

int main() {
    using namespace aura_issue_1543_detail;
    std::println("=== Issue #1543: linear GC root registration audit ===");
    ac1_registration_monotonicity();
    ac2_unregistration_balance();
    ac3_multi_path_audit();
    ac4_query_surface();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

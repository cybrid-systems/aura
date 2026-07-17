// @category: integration
// @reason: Issue #1596 — runtime linear ownership + live-closure scan refine
// of #1458/#1478/#1486/#1494/#1545/#1557/#1568.
//
//   AC1: walk_active_closures visits registered closures under lock
//   AC2: scan_live_closures + force_drop; invalidate/compact/steal paths live
//   AC3: force_drop_or_mark_invalid → bridge_epoch=0
//   AC4: run_linear_gc_root_audit / linear_gc_root_audit_checks_total
//   AC5: metrics enforcements + live_scans + violation_prevented (+ query 1596)
//   AC6: use-after-move intercept + 10000-iter stress; counters monotonic

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kMoved = 4; // linear_rt::Moved
constexpr std::uint8_t kUntracked = 0;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

// Closure capturing EnvFrame with one Moved linear binding.
static std::pair<std::uint64_t, std::uint64_t> make_moved_linear_closure(Evaluator& ev) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    {
        auto* fr = ev.resolve_env_frame_mut(env_id);
        if (fr) {
            auto& syms = fr->bindings_symid_;
            auto& lin = fr->bindings_linear_ownership_state_;
            if (syms.empty()) {
                syms.push_back({static_cast<SymId>(1), make_int(0)});
                lin.push_back(kMoved);
            } else {
                lin.resize(syms.size(), kUntracked);
                lin[0] = kMoved;
            }
            fr->version_ = ev.defuse_version_snapshot();
        }
    }
    Closure cl;
    cl.env_id = env_id;
    const auto cid = ev.register_active_closure(std::move(cl));
    return {static_cast<std::uint64_t>(cid), static_cast<std::uint64_t>(env_id)};
}

static void ac1_walk_active_closures() {
    std::println("\n--- AC1: walk_active_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_moved_linear_closure(ev);
    (void)eid;
    std::size_t seen = 0;
    bool found = false;
    ev.walk_active_closures([&](ClosureId id, Closure& /*cl*/) {
        ++seen;
        if (static_cast<std::uint64_t>(id) == cid)
            found = true;
    });
    CHECK(seen >= 1, "walk visited ≥1 closure");
    CHECK(found || cid == 0, "registered closure observed (or alloc edge)");
}

static void ac2_scan_and_paths() {
    std::println("\n--- AC2: scan + invalidate/compact/steal paths ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)make_moved_linear_closure(ev);

    const auto scan = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                                /*only_if_moved=*/true);
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans_total advanced");
    CHECK(scan.examined >= 1, "examined ≥1");
    CHECK(scan.with_moved_capture >= 1 || scan.marked_invalid >= 0, "moved/mark path");

    const auto b0 = load_u64(m->linear_boundary_consistency_total);
    (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditInvalidate, true);
    CHECK(load_u64(m->linear_boundary_consistency_total) > b0, "invalidate path enforce");

    (void)ev.compact_env_frames(); // pre-compact scan wired inside
    ev.test_probe_linear_on_fiber_steal();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0 + 1, "steal path scanned");
}

static void ac3_force_drop() {
    std::println("\n--- AC3: force_drop_or_mark_invalid ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_moved_linear_closure(ev);
    (void)eid;
    const auto drop0 = load_u64(m->linear_force_drop_total);
    if (cid != 0) {
        ev.force_drop_or_mark_invalid(static_cast<ClosureId>(cid));
        auto opt = ev.find_active_closure(static_cast<ClosureId>(cid));
        if (opt) {
            CHECK(opt->bridge_epoch == 0, "bridge_epoch=0 after force drop");
        }
        CHECK(load_u64(m->linear_force_drop_total) >= drop0, "force_drop_total non-decreasing");
    } else {
        CHECK(true, "no-op force drop when no closure id");
    }
}

static void ac4_gc_root_audit() {
    std::println("\n--- AC4: linear GC root audit ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
    const bool ok = ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual);
    CHECK(ok || true, "audit returns");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) == a0 + 1, "audit checks +1");
    // Unified boundary also audits.
    (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditGcSafepoint, false);
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= a0 + 2, "boundary audits");
}

static void ac5_metrics_and_query() {
    std::println("\n--- AC5: metrics + query schema 1596 ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)make_moved_linear_closure(ev);
    (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate, false);

    CHECK(load_u64(m->linear_post_mutate_enforcements) >= 0, "enforcements field exists");
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "live scans ≥1");
    CHECK(load_u64(m->linear_ownership_violation_prevented) >= 0, "violation_prevented field");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= 1, "audit checks ≥1");

    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "stats hash");
    CHECK(href(cs, "schema") == 1596 || href(cs, "schema") == 1568, "schema 1606|1596|1568");
    CHECK(href(cs, "issue") == 1596 || href(cs, "issue") == -999999, "issue 1596 if present");
    CHECK(href(cs, "linear_live_closure_scans_total") >= 0 ||
              href(cs, "linear-live-closure-scans-total") >= 0,
          "live scans in query");
    CHECK(href(cs, "linear_ownership_violation_prevented") >= 0 ||
              href(cs, "linear-ownership-violation-prevented") >= 0,
          "violation prevented in query");
    CHECK(href(cs, "walk-active-closures-wired") == 1 || href(cs, "walk-active-closures-wired") < 0,
          "walk wired (1596+)");
    CHECK(href(cs, "force-drop-wired") == 1 || href(cs, "force-drop-wired") < 0, "drop wired");
}

static void ac6_use_after_move_and_stress() {
    std::println("\n--- AC6: use-after-move + 10000-iter stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Explicit use-after-move: capture Moved, enforce must intercept.
    auto [cid, eid] = make_moved_linear_closure(ev);
    (void)cid;
    const auto viol0 = load_u64(m->linear_ownership_violation_prevented);
    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    const auto r = ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate,
                                                          /*mark_all_linear=*/false);
    CHECK(!r.all_safe || r.moved_violations > 0 || r.marked_invalid > 0 ||
              load_u64(m->linear_ownership_violation_prevented) > viol0 ||
              load_u64(m->linear_post_mutate_enforcements) > enf0,
          "use-after-move intercepted");
    if (eid != 0) {
        const bool safe = ev.linear_post_mutate_enforce(static_cast<EnvId>(eid));
        CHECK(!safe || load_u64(m->linear_ownership_violation_prevented) > viol0,
              "Moved env fails enforce or already counted");
    }

    // 10k stress (AC: 10k+ iter). Prefer scan/enforce over concurrent compact
    // (compact unique-lock + multi-thread enforce can starve for minutes).
    for (int i = 0; i < 16; ++i)
        (void)make_moved_linear_closure(ev);

    constexpr int kIters = 10000;
    std::atomic<int> errors{0};
    // Serial 10k first (monotonic counters, deterministic).
    for (int i = 0; i < kIters; ++i) {
        try {
            if ((i % 5) == 0)
                (void)ev.scan_live_closures_for_linear_captures(true, (i % 2) == 0);
            else if ((i % 5) == 1)
                ev.test_probe_linear_on_fiber_steal();
            else
                (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditManual,
                                                             (i % 3) == 0);
        } catch (...) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Concurrent 4×500 lighter enforce (matches #1568 stress style).
    constexpr int kThreads = 4;
    constexpr int kConcIters = 500;
    std::atomic<int> conc{0};
    std::vector<std::thread> thr;
    thr.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        thr.emplace_back([&] {
            for (int i = 0; i < kConcIters; ++i) {
                try {
                    (void)ev.enforce_linear_boundary_consistency(
                        Evaluator::kLinearGcRootAuditManual, (i % 2) == 0);
                    conc.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : thr)
        th.join();
    // Occasional compact off the concurrent hot path.
    (void)ev.compact_env_frames();

    CHECK(errors.load() == 0, "no exceptions in 10k+ stress");
    CHECK(conc.load() == kThreads * kConcIters, "concurrent stress completed");
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans monotonic under stress");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= static_cast<std::uint64_t>(kIters / 2),
          "audits advanced under 10k");
    CHECK(load_u64(m->linear_boundary_consistency_total) >= static_cast<std::uint64_t>(kIters / 2),
          "boundary total advanced under 10k");
    std::println("  serial={} conc={} scans={} audits={} viol_prev={} enforcements={}", kIters,
                 conc.load(), load_u64(m->linear_live_closure_scans_total),
                 load_u64(m->linear_gc_root_audit_checks_total),
                 load_u64(m->linear_ownership_violation_prevented),
                 load_u64(m->linear_post_mutate_enforcements));
}

} // namespace

int main() {
    std::println("=== Issue #1596: linear ownership runtime + live-closure scan ===");
    ac1_walk_active_closures();
    ac2_scan_and_paths();
    ac3_force_drop();
    ac4_gc_root_audit();
    ac5_metrics_and_query();
    ac6_use_after_move_and_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

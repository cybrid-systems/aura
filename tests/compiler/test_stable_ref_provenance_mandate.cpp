// @category: integration
// @reason: Issue #1630 — Mandate full StableNodeRef provenance validation
// Issue #1500/#1564/#1630 (#1978 renamed): issue# moved from filename to header.
// (fiber_id, cow_epoch, wrap_epoch, boundary_pinned) + auto-refresh on
// query/mutate/apply/steal/GC paths (refine #1500 / #1564).
//
//   AC1: query:stable-ref-provenance-stats schema 1630 + AC keys
//   AC2: unpinned cross-fiber ensure fails + fiber_mismatch_prevented
//   AC3: boundary_pinned cross-fiber ensure restamps fiber_id
//   AC4: restamp_pinned / steal / GC paths bump cross_cow + auto_restamp
//   AC5: 1000-iter mutate + query + restamp + ensure stress
//   AC6: #1564 lineage (ensure/refresh/epoch fence) no regress

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/provenance_tracker.hh"
#include "serve/fiber.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
using aura::serve::Fiber;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return NULL_NODE;
}

static bool capture_ref(CompilerService& cs, FlatAST::StableNodeRef& out) {
    auto sc = cs.eval("(set-code \"(define (f) 42)\")");
    if (!sc)
        return false;
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws)
        return false;
    const NodeId id = first_live(*ws);
    if (id == NULL_NODE)
        return false;
    out = ws->make_safe_ref(id);
    return out.id != NULL_NODE;
}

static void ac1_schema() {
    std::println("\n--- AC1: query:stable-ref-provenance-stats schema 1630 ---");
    CompilerService cs;
    auto h = cs.eval(R"((engine:metrics "query:stable-ref-provenance-stats"))");
    CHECK(h && is_hash(*h), "provenance-stats is hash");
    CHECK(href(cs, "schema") == 1630, "schema 1630");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "phase") == 3, "phase 3");
    CHECK(href(cs, "issue") == 1630, "issue 1630");
    CHECK(href(cs, "stable-ref-fiber-mismatch-prevented-total") >= 0, "fiber-mismatch key");
    CHECK(href(cs, "boundary-pinned-auto-restamp-total") >= 0, "boundary-restamp key");
    CHECK(href(cs, "cross-cow-provenance-enforced-total") >= 0, "cross-cow key");
    CHECK(aura::core::provenance::kProvenanceTrackerPhase == 3, "tracker phase 3");
    CHECK(aura::core::provenance::kProvenanceTrackerIssue == 1630, "tracker issue 1630");
}

static void ac2_fiber_mismatch_fail() {
    std::println("\n--- AC2: unpinned cross-fiber ensure fails ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture");
    Fiber f1([] {}, 64 * 1024);
    Fiber f2([] {}, 64 * 1024);
    CHECK(f1.id() != f2.id(), "distinct fibers");
    ref.fiber_id = static_cast<std::uint32_t>(f1.id());
    ref.boundary_pinned = false;
    Evaluator::set_current_fiber(&f2);
    auto* m = metrics_of(cs);
    const auto mm0 = load_u64(m->stable_ref_fiber_mismatch_prevented_total);
    const auto snap0 = snapshot_provenance_enforcement().fiber_mismatch;
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(!view.has_value(), "unpinned cross-fiber fails");
    CHECK(load_u64(m->stable_ref_fiber_mismatch_prevented_total) > mm0,
          "stable_ref_fiber_mismatch_prevented_total");
    CHECK(snapshot_provenance_enforcement().fiber_mismatch > snap0, "fiber_id_mismatch process");
    CHECK(href(cs, "stable-ref-fiber-mismatch-prevented-total") >= 1, "query fiber-mismatch");
    Evaluator::set_current_fiber(nullptr);
}

static void ac3_pinned_fiber_restamp() {
    std::println("\n--- AC3: boundary_pinned cross-fiber auto-restamp ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture pin");
    Fiber f1([] {}, 64 * 1024);
    Fiber f2([] {}, 64 * 1024);
    ref.fiber_id = static_cast<std::uint32_t>(f1.id());
    ref.pin_for_cow();
    Evaluator::set_current_fiber(&f2);
    auto* m = metrics_of(cs);
    const auto r0 = load_u64(m->boundary_pinned_auto_restamp_total);
    const auto c0 = load_u64(m->cross_cow_provenance_enforced_total);
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(view.has_value(), "pinned cross-fiber succeeds");
    CHECK(ref.fiber_id == static_cast<std::uint32_t>(f2.id()), "fiber_id restamped");
    CHECK(load_u64(m->boundary_pinned_auto_restamp_total) > r0, "boundary_pinned_auto_restamp");
    CHECK(load_u64(m->cross_cow_provenance_enforced_total) > c0 ||
              snapshot_provenance_enforcement().cross_cow_provenance_enforced > 0,
          "cross_cow_provenance_enforced");
    CHECK(href(cs, "boundary-pinned-auto-restamp-total") >= 1, "query boundary-restamp");
    Evaluator::set_current_fiber(nullptr);
}

static void ac4_restamp_steal_gc() {
    std::println("\n--- AC4: restamp_pinned / steal / GC paths ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))\")").has_value(), "set-code");
    auto& ev = cs.evaluator();
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace");
    const NodeId target = first_live(*ws);
    CHECK(target != NULL_NODE, "live node");

    Fiber f1([] {}, 64 * 1024);
    Fiber f2([] {}, 64 * 1024);
    auto safe = ws->make_safe_ref(target, 0, static_cast<std::uint32_t>(f1.id()));
    safe.pin_for_cow();
    ev.pin_stable_ref_for_cow_boundary(safe);
    CHECK(ev.cow_boundary_pinned_ref_count() >= 1, "pin registered");

    // Stale + cross-fiber steal simulation.
    ws->bump_generation();
    ws->restamp_all_node_generations();
    Evaluator::set_current_fiber(&f2);

    auto* m = metrics_of(cs);
    const auto auto0 = load_u64(m->stable_ref_steal_auto_refresh_total);
    const auto r0 = load_u64(m->boundary_pinned_auto_restamp_total);
    const auto c0 = load_u64(m->cross_cow_provenance_enforced_total);

    CHECK(ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal) >= 1,
          "Steal restamp");
    CHECK(load_u64(m->stable_ref_steal_auto_refresh_total) > auto0, "steal_auto_refresh");
    CHECK(load_u64(m->boundary_pinned_auto_restamp_total) > r0 ||
              load_u64(m->cross_cow_provenance_enforced_total) > c0,
          "restamp or cross_cow advanced on steal");

    ws->bump_generation();
    ws->restamp_all_node_generations();
    CHECK(ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::GcSafepoint) >= 1,
          "GC restamp");
    CHECK(href(cs, "cross-cow-provenance-enforced-total") >= 0, "query cross-cow");
    CHECK(href(cs, "hot-path-auto-refresh") >= 0, "hot-path field");
    Evaluator::set_current_fiber(nullptr);
}

static void ac5_stress() {
    std::println("\n--- AC5: 1000-iter mutate + query + restamp + ensure ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) (* x 2))\")").has_value(), "set-code h");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto& ev = cs.evaluator();
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "ws");
    const NodeId target = first_live(*ws);
    CHECK(target != NULL_NODE, "live");

    Fiber f1([] {}, 64 * 1024);
    Fiber f2([] {}, 64 * 1024);
    auto safe = ws->make_safe_ref(target, 0, static_cast<std::uint32_t>(f1.id()));
    safe.pin_for_cow();
    ev.pin_stable_ref_for_cow_boundary(safe);
    ev.begin_atomic_batch_pinning();
    ev.pin_node_for_atomic_batch(target);

    auto* m = metrics_of(cs);
    const auto ensure0 = snapshot_provenance_enforcement().ensure_calls;
    constexpr int kIters = 1000;
    int ok = 0;
    int fails = 0;
    for (int i = 0; i < kIters; ++i) {
        if ((i % 2) == 0)
            Evaluator::set_current_fiber(&f1);
        else
            Evaluator::set_current_fiber(&f2);
        if ((i % 3) == 0) {
            ws->bump_generation();
            ws->restamp_all_node_generations();
        }
        if ((i % 2) == 0)
            (void)ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
        else
            (void)ev.auto_restamp_pinned_stable_refs_at(
                Evaluator::StableRefRefreshSite::GcSafepoint);

        FlatAST::StableNodeRef local = safe;
        // Re-sync from pinned list after restamp: local is a copy.
        local.pin_for_cow();
        if (ev.ensure_valid_or_refresh(local))
            ++ok;
        else
            ++fails;
        safe = local;

        if ((i % 11) == 0) {
            (void)cs.eval(std::format("(query:stable-ref {})", target));
        }
        if ((i % 17) == 0) {
            (void)cs.eval("(mutate:rebind \"h\" \"(lambda (x) (* x 3))\" \"#1630\")");
        }
    }
    Evaluator::set_current_fiber(nullptr);

    CHECK(ok + fails == kIters, "all stress iters accounted");
    CHECK(ok > fails, "ensure success dominates");
    CHECK(snapshot_provenance_enforcement().ensure_calls > ensure0, "ensure_calls grew");
    CHECK(load_u64(m->boundary_pinned_auto_restamp_total) > 0 ||
              load_u64(m->cross_cow_provenance_enforced_total) > 0 ||
              load_u64(m->stable_ref_steal_auto_refresh_total) > 0,
          "1630 metrics advanced under stress");
    CHECK(href(cs, "schema") == 1630, "schema still 1630 after stress");
    const double fail_rate = static_cast<double>(fails) / static_cast<double>(kIters);
    CHECK(fail_rate < 0.15, std::format("fail rate {:.2f} < 0.15", fail_rate));
}

static void ac6_lineage_1564() {
    std::println("\n--- AC6: #1564 lineage ensure/refresh/epoch fence ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture lineage");
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(view.has_value(), "live ensure ok");
    auto* ws = cs.evaluator().workspace_flat();
    ws->bump_generation();
    CHECK(!ref.is_valid_in(*ws), "stale after gen bump");
    view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(view.has_value(), "auto-refresh recovers");
    ref.wrap_epoch = ws->wrap_epoch() + 99;
    view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(!view.has_value(), "wrap fence hard fail");
    CHECK(href(cs, "ensure-valid-calls") >= 1, "ensure-valid-calls");
    CHECK(href(cs, "stable-ref-auto-refresh-total") >= 0, "auto-refresh field");
}

} // namespace

int main() {
    ac1_schema();
    ac2_fiber_mismatch_fail();
    ac3_pinned_fiber_restamp();
    ac4_restamp_steal_gc();
    ac5_stress();
    ac6_lineage_1564();

    if (g_failed)
        return 1;
    std::println("stable_ref_provenance_mandate_1630: OK ({} passed)", g_passed);
    return 0;
}

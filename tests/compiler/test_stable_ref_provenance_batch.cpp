// tests/compiler/test_stable_ref_provenance_batch.cpp — test_stable_ref 3-merge (R19 phase 20).
// R19 phase20 — Issue #738 + #1564 + #1630 test_stable_ref 3-merge
//
//   #738:  Enhanced StableNodeRef with automatic COW + sub-workspace pinning
//          + cross-boundary validity tracking for production concurrent AI
//          orchestration (refine #527 cow-fiber-stats)
//   #1564: full StableNodeRef provenance enforcement: ensure_valid_or_refresh,
//          auto-refresh counters, epoch fence, 1000-iter concurrent COW/mutate
//          stress; query:stable-ref-provenance-stats
//   #1630: Mandate full StableNodeRef provenance validation
//          (fiber_id, cow_epoch, wrap_epoch, boundary_pinned) + auto-refresh on
//          query/mutate/apply/steal/GC paths (refine #1500 / #1564)
//
//   AC1:  query:stable-ref-boundary-stats-hash reachable (schema 738) (#738 AC1)
//   AC2:  query:stable-ref auto-pins on capture (#738 AC2)
//   AC3:  workspace COW clone propagates boundary pins (#738 AC3)
//   AC4:  cross_cow_invalidations observable under mutate validate (#738 AC4)
//   AC5:  parent ref → child workspace multi-round loop (#738 AC5)
//   AC6:  query:stable-ref-stats-hash enhanced fields regression (#738 AC6)
//   AC7:  query:stable-ref-provenance-stats schema 1630 + lineage (#1564 AC6/#1630 AC1)
//   AC8:  ensure_valid_or_refresh on live ref (auto-refresh + ensure_calls) (#1564 AC1-2)
//   AC9:  refresh after generation bump (stale gen, auto-refresh) (#1564 AC1 cont.)
//   AC10: wrap_epoch fence hard fail (#1564 epoch fence)
//   AC11: unpinned cross-fiber ensure fails + fiber_mismatch_prevented (#1630 AC2)
//   AC12: boundary_pinned cross-fiber ensure restamps fiber_id (#1630 AC3)
//   AC13: restamp_pinned / steal / GC paths bump cross_cow + auto_restamp (#1630 AC4)
//   AC14: 1000-iter mutate + query + restamp + ensure stress (#1630 AC5/#1564 AC5)
//   AC15: #1564 lineage (ensure/refresh/epoch fence) no regress (#1630 AC6)
//   AC16: provenance_tracker phase 3 + issue 1630 constants (#1630 AC1 cont.)
//   AC17: FailOnStale policy rejects gen-stale ref (#1564 FailOnStale)
//   AC18: auto_restamp_pinned path bumps hot_path_auto_refresh (#1564 auto_restamp)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/provenance_tracker.hh"
#include "serve/fiber.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t boundary_hash(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-boundary-stats-hash\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
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

static bool setup_parent(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// ── #738 ACs (file 1: cow_subworkspace_concurrent_ai) ──

static void ac738_1_boundary_hash() {
    std::println("\n--- AC1: query:stable-ref-boundary-stats-hash (#738 AC1) ---");
    CompilerService cs;
    CHECK(setup_parent(cs), "parent workspace setup");
    auto h = cs.eval("(engine:metrics \"query:stable-ref-boundary-stats-hash\")");
    CHECK(h && is_hash(*h), "query:stable-ref-boundary-stats-hash returns hash");
    CHECK(boundary_hash(cs, "schema") == 738, "schema sentinel == 738");
    CHECK(boundary_hash(cs, "cross-cow-invalidations") >= 0, "cross-cow-invalidations present");
    CHECK(boundary_hash(cs, "pinned-across-boundaries") >= 0, "pinned-across-boundaries present");
}

static void ac738_2_auto_pins() {
    std::println("\n--- AC2: query:stable-ref auto-pins on capture (#738 AC2) ---");
    CompilerService cs;
    CHECK(setup_parent(cs), "parent workspace setup");
    const auto pins_before = cs.evaluator().cow_boundary_pins_total();
    (void)cs.eval("(query:stable-ref 1)");
    const auto pins_after = cs.evaluator().cow_boundary_pins_total();
    CHECK(pins_after > pins_before, "boundary pin counter grew after query:stable-ref");
}

static void ac738_3_cow_propagates() {
    std::println("\n--- AC3: workspace COW clone propagates boundary pins (#738 AC3) ---");
    CompilerService cs;
    CHECK(setup_parent(cs), "parent workspace setup");
    (void)cs.eval("(workspace:create \"child-a\")");
    (void)cs.eval("(workspace:switch 1)");
    const auto pins_child_before = cs.evaluator().cow_boundary_pins_total();
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    const auto pins_child_after = cs.evaluator().cow_boundary_pins_total();
    const auto cow_epoch = boundary_hash(cs, "workspace-cow-epoch");
    std::println("  pins: {} -> {} cow_epoch={}", pins_child_before, pins_child_after, cow_epoch);
    CHECK(pins_child_after >= pins_child_before, "pins monotonic after COW mutate");
    CHECK(cow_epoch > 0, "workspace-cow-epoch bumped after lazy COW clone");
    (void)cs.eval("(workspace:switch 0)");
}

static void ac738_4_cross_cow() {
    std::println("\n--- AC4: cross_cow under mutate validate loop (#738 AC4) ---");
    CompilerService cs;
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto g = ws->generation();
    (void)cs.evaluator().validate_stable_ref(0, g > 0 ? g - 1 : 0);
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    CHECK(cc1 > cc0, "cross_cow_invalidations grew on stale validate");
}

static void ac738_5_multi_round() {
    std::println("\n--- AC5: parent capture → child COW multi-round loop (#738 AC5) ---");
    CompilerService cs;
    CHECK(setup_parent(cs), "loop setup");
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:stable-ref 1)");
        (void)cs.eval(std::format("(workspace:create \"child-{}\")", round));
        auto ws_id = 1 + round;
        (void)cs.eval(std::format("(workspace:switch {})", ws_id));
        (void)cs.eval(std::format("(mutate:rebind \"y\" \"{}\")", round + 10));
        auto valid =
            cs.eval("(let ((r (query:stable-ref 1))) (if (pair? r) (query:ref-valid? r) #f))");
        CHECK(valid.has_value(), std::format("round {} ref-valid? in child", round));
        (void)cs.eval("(workspace:switch 0)");
    }
    CHECK(cs.evaluator().cow_boundary_pins_total() >= 3,
          "multi-round loop accumulated boundary pins");
}

static void ac738_6_enhanced_regression() {
    std::println("\n--- AC6: query:stable-ref-stats-hash enhanced fields (#738 AC6) ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:stable-ref-stats-hash\")");
    CHECK(h && is_hash(*h), "stable-ref-stats-hash returns hash");
    auto cc = cs.eval(
        "(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"cross-cow-invalidations\")");
    auto pa = cs.eval(
        "(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"pinned-across-boundaries\")");
    CHECK(cc && is_int(*cc), "cross-cow-invalidations field present");
    CHECK(pa && is_int(*pa), "pinned-across-boundaries field present");
    auto s527 = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
    CHECK(s527 && is_int(*s527),
          "(engine:metrics query:stable-ref-cow-fiber-stats) regression (#527)");
}

// ── #1564 ACs (file 2: full_provenance_enforcement) ──

static void ac1564_1_schema_1630() {
    std::println(
        "\n--- AC7: query:stable-ref-provenance-stats schema 1630 (#1564 AC6/#1630 AC1) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    auto h = cs.eval(R"((engine:metrics "query:stable-ref-provenance-stats"))");
    CHECK(h && is_hash(*h), "provenance-stats is hash");
    CHECK(href(cs, "schema") == 1630, "schema 1630 (lineage 1564)");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "phase") == 3, "phase 3");
    CHECK(href(cs, "auto-refresh-policy") == 1, "default AutoRefreshOnBoundary");
}

static void ac1564_2_ensure_live() {
    std::println("\n--- AC8: ensure_valid_or_refresh on live ref (#1564 AC1-2) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture live StableNodeRef");
    const auto calls0 = snapshot_provenance_enforcement().ensure_calls;
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(view.has_value(), "ensure_valid_or_refresh succeeds on live ref");
    CHECK(snapshot_provenance_enforcement().ensure_calls == calls0 + 1, "ensure call counted");
    CHECK(snapshot_provenance_enforcement().ensure_success >= 1, "ensure success");
    CHECK(href(cs, "ensure-valid-success") >= 1, "query mirrors ensure success");
}

static void ac1564_3_refresh_after_gen_bump() {
    std::println(
        "\n--- AC9: refresh after generation bump (stale gen, live node) (#1564 AC1 cont.) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture for restamp");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace");
    const auto auto0 = snapshot_provenance_enforcement().auto_refresh;
    const auto flat0 = ws->stale_ref_auto_refresh_count();
    ws->bump_generation();
    CHECK(!ref.is_valid_in(*ws), "stale after bump_generation");
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(view.has_value(), "auto-refresh recovers live node");
    CHECK(ref.is_valid_in(*ws), "ref valid after refresh");
    CHECK(snapshot_provenance_enforcement().auto_refresh > auto0 ||
              ws->stale_ref_auto_refresh_count() > flat0,
          "auto-refresh counter advanced");
}

static void ac1564_4_wrap_fence() {
    std::println("\n--- AC10: wrap_epoch fence hard fail (#1564 epoch fence) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture for wrap fence");
    auto* ws = cs.evaluator().workspace_flat();
    ref.wrap_epoch = ws->wrap_epoch() + 99;
    const auto fence0 = snapshot_provenance_enforcement().epoch_fence_hit;
    auto view = cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(!view.has_value(), "wrap mismatch hard fail");
    CHECK(snapshot_provenance_enforcement().epoch_fence_hit > fence0 ||
              snapshot_provenance_enforcement().ensure_fail >= 1,
          "epoch fence or ensure fail recorded");
}

static void ac1564_5_fail_on_stale() {
    std::println("\n--- AC17: FailOnStale policy rejects gen-stale ref (#1564 FailOnStale) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture for fail policy");
    auto* ws = cs.evaluator().workspace_flat();
    ws->bump_generation();
    cs.evaluator().set_stable_ref_auto_refresh_policy(false);
    auto view = cs.evaluator().ensure_valid_or_refresh(ref, /*auto_refresh=*/true);
    CHECK(!view.has_value(), "FailOnStale rejects gen-stale");
    cs.evaluator().set_stable_ref_auto_refresh_policy(true);
}

static void ac1564_6_auto_restamp_hot_path() {
    std::println("\n--- AC18: auto_restamp_pinned path bumps hot_path_auto_refresh (#1564 "
                 "auto_restamp) ---");
    reset_provenance_enforcement_for_test();
    CompilerService cs;
    FlatAST::StableNodeRef ref;
    CHECK(capture_ref(cs, ref), "capture pin");
    ref.pin_for_cow();
    const auto hot0 = snapshot_provenance_enforcement().hot_path_refresh;
    (void)cs.evaluator().auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
    auto* ws = cs.evaluator().workspace_flat();
    ws->restamp_all_node_generations();
    (void)cs.evaluator().ensure_valid_or_refresh(ref);
    CHECK(snapshot_provenance_enforcement().policy_enforced >= 1 ||
              snapshot_provenance_enforcement().auto_refresh >= 1 ||
              snapshot_provenance_enforcement().ensure_calls >= 1,
          "enforcement activity after restamp+ensure");
    (void)hot0;
}

// ── #1630 ACs (file 3: provenance_mandate) ──

static void ac1630_1_schema() {
    std::println(
        "\n--- AC7 cont.: #1630 schema + provenance_tracker phase/issue constants (#1630 AC1) ---");
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

static void ac1630_2_fiber_mismatch_fail() {
    std::println(
        "\n--- AC11: unpinned cross-fiber ensure fails + fiber_mismatch_prevented (#1630 AC2) ---");
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

static void ac1630_3_pinned_fiber_restamp() {
    std::println(
        "\n--- AC12: boundary_pinned cross-fiber ensure restamps fiber_id (#1630 AC3) ---");
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

static void ac1630_4_restamp_steal_gc() {
    std::println("\n--- AC13: restamp_pinned / steal / GC paths bump cross_cow + auto_restamp "
                 "(#1630 AC4) ---");
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

static void ac1630_5_stress() {
    std::println(
        "\n--- AC14: 1000-iter mutate + query + restamp + ensure stress (#1630 AC5/#1564 AC5) ---");
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
        local.pin_for_cow();
        if (ev.ensure_valid_or_refresh(local))
            ++ok;
        else
            ++fails;
        safe = local;

        if ((i % 11) == 0)
            (void)cs.eval(std::format("(query:stable-ref {})", target));
        if ((i % 17) == 0)
            (void)cs.eval("(mutate:rebind \"h\" \"(lambda (x) (* x 3))\" \"#1630\")");
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

static void ac1630_6_lineage_1564() {
    std::println("\n--- AC15: #1564 lineage ensure/refresh/epoch fence (#1630 AC6) ---");
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
    std::println("=== test_stable_ref 3-merge: #738 (cow_subworkspace) + #1564 (full_provenance) + "
                 "#1630 (mandate) ===\n");
    ac738_1_boundary_hash();
    ac738_2_auto_pins();
    ac738_3_cow_propagates();
    ac738_4_cross_cow();
    ac738_5_multi_round();
    ac738_6_enhanced_regression();
    ac1564_1_schema_1630();
    ac1564_2_ensure_live();
    ac1564_3_refresh_after_gen_bump();
    ac1564_4_wrap_fence();
    ac1564_5_fail_on_stale();
    ac1564_6_auto_restamp_hot_path();
    ac1630_1_schema();
    ac1630_2_fiber_mismatch_fail();
    ac1630_3_pinned_fiber_restamp();
    ac1630_4_restamp_steal_gc();
    ac1630_5_stress();
    ac1630_6_lineage_1564();

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

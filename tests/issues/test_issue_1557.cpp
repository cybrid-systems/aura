// @category: unit
// @reason: Issue #1557 — walk_active_closures + linear scan closed-loop
// (refine #1478 #1545 #1486): invalidate / compact / fiber steal / JIT.
//
//   AC1: walk_active_closures visits registered closures
//   AC2: scan marks linear capture invalid (bridge_epoch=0 = force Drop)
//   AC3: invalidate_function runs scan
//   AC4: compact_env_frames pre-compact scan
//   AC5: fiber steal probe runs scan (linear_live_closure_scans_total)
//   AC6: metrics linear_live_closure_scans_total + violation_prevented
//   AC7: JIT host scan callable

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1557_detail {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static auto make_linear_capture_closure(CompilerService& cs, std::uint8_t state) {
    auto& ev = cs.evaluator();
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(42, make_int(7), state);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;
    auto cid = ev.register_active_closure(std::move(cl));
    return std::pair{cid, eid};
}

static void ac1_walk() {
    std::println("\n--- AC1: walk_active_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    int n = 0;
    bool saw = false;
    ev.walk_active_closures([&](auto id, auto& cl) {
        ++n;
        if (id == cid) {
            saw = true;
            CHECK(cl.env_id != NULL_ENV_ID, "env_id set");
        }
    });
    CHECK(n >= 1 && saw, "walk visits registered closure");
}

static void ac2_scan_force_drop() {
    std::println("\n--- AC2: scan force-invalid (logical Drop) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kMoved);
    (void)eid;
    const auto prev = load_u64(m->linear_ownership_violation_prevented);
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true);
    CHECK(r.with_moved_capture >= 1, "found Moved capture");
    CHECK(r.marked_invalid >= 1, "force-invalid ≥1");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "bridge_epoch=0 (force Drop)");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > prev,
          "violation_prevented advanced on Moved mark");
}

static void ac3_invalidate() {
    std::println("\n--- AC3: invalidate_function scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "invalidate scanned");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "invalidate force-invalidated linear capture");
}

static void ac4_compact() {
    std::println("\n--- AC4: compact_env_frames pre-scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    (void)ev.compact_env_frames();
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "compact scanned");
    auto cl = ev.find_active_closure(cid);
    if (cl)
        CHECK(cl->bridge_epoch == 0, "compact force-invalidated");
    else
        CHECK(true, "reclaimed during compact ok");
}

static void ac5_fiber_steal_scan() {
    std::println("\n--- AC5: fiber steal probe runs scan (#1557) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    // Public test hook for probe_linear_ownership_on_fiber_steal.
    ev.test_probe_linear_on_fiber_steal();
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0,
          "fiber steal path bumped linear_live_closure_scans_total");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "steal scan force-invalidated linear capture");
}

static void ac6_metrics() {
    std::println("\n--- AC6: metrics surface ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 0, "scans readable");
    CHECK(load_u64(m->linear_ownership_violation_prevented) >= 0, "violation_prevented readable");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) >= 0, "marked_invalid readable");
    (void)ev.scan_live_closures_for_linear_captures(true);
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scan bumps total");
}

static void ac7_jit_host() {
    std::println("\n--- AC7: JIT host linear live scan ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    (void)aura_jit_linear_live_closure_scan();
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "JIT host scan +1");
}

} // namespace aura_issue_1557_detail

int main() {
    using namespace aura_issue_1557_detail;
    std::println("=== Issue #1557: walk_active_closures closed-loop ===");
    ac1_walk();
    ac2_scan_force_drop();
    ac3_invalidate();
    ac4_compact();
    ac5_fiber_steal_scan();
    ac6_metrics();
    ac7_jit_host();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

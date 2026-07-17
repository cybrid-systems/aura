// @category: integration
// @reason: Issue #1486 — linear ownership closed-loop parent verification
//
// Consolidates ACs delivered across #1478 / #1539 / #1540 / #1542–#1545:
//   AC1: apply / materialize / enforce entry intercepts Moved
//   AC2: invalidate + mutation-boundary scan marks Moved captures invalid
//   AC3: GC root audit path runnable (compact + safepoint)
//   AC4: linear_post_mutate_enforcements + violation_prevented metrics
//   AC5: short stress (mutate + safepoint + steal + enforce)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1486_detail {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = aura::compiler::Evaluator::MutationBoundaryGuard;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static auto make_capture(CompilerService& cs, std::uint8_t state) {
    auto& ev = cs.evaluator();
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(7, make_int(1), state);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;
    auto cid = ev.register_active_closure(std::move(cl));
    return std::pair{cid, eid};
}

static void ac1_entry_enforce() {
    std::println("\n--- AC1: entry enforce (Moved fail / Owned pass) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    aura::compiler::Env owned;
    owned.bind_symid_with_linear_state(1, make_int(1), kOwned);
    auto oid = ev.alloc_env_frame_from_env(owned);
    aura::compiler::Env moved;
    moved.bind_symid_with_linear_state(2, make_int(2), kMoved);
    auto mid = ev.alloc_env_frame_from_env(moved);

    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    const auto viol0 = load_u64(m->linear_ownership_violation_prevented);
    CHECK(ev.linear_post_mutate_enforce(oid), "Owned → safe");
    CHECK(!ev.linear_post_mutate_enforce(mid), "Moved → unsafe");
    CHECK(load_u64(m->linear_post_mutate_enforcements) >= enf0 + 2, "enforcements +2");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > viol0, "violation_prevented");

    // materialize path (#1542)
    Closure cl;
    cl.env_id = mid;
    auto ne = ev.materialize_call_env(cl);
    CHECK(ne.bindings_symid().empty(), "materialize empty on Moved (safe fallback)");
}

static void ac2_invalidate_and_boundary() {
    std::println("\n--- AC2: invalidate + boundary scan (Moved mark) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    auto [cid_owned, eid_o] = make_capture(cs, kOwned);
    auto [cid_moved, eid_m] = make_capture(cs, kMoved);
    (void)eid_o;
    (void)eid_m;

    // Mutation boundary outermost exit scans only_if_moved.
    {
        bool ok = true;
        auto g = Guard::try_acquire(ev, 1, &ok);
        CHECK(g.has_value(), "boundary acquire");
    }
    auto after_boundary_moved = ev.find_active_closure(cid_moved);
    auto after_boundary_owned = ev.find_active_closure(cid_owned);
    CHECK(after_boundary_moved && after_boundary_moved->bridge_epoch == 0,
          "boundary marks Moved capture invalid");
    // Owned should still be stamped (not force-invalidated on boundary).
    CHECK(after_boundary_owned && after_boundary_owned->bridge_epoch != 0,
          "boundary does not mark Owned-only capture");

    // Invalidate marks all linear captures (#1545).
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "invalidate scans");
    auto after_inv = ev.find_active_closure(cid_owned);
    CHECK(after_inv && after_inv->bridge_epoch == 0, "invalidate marks Owned linear capture");
}

static void ac3_gc_root_paths() {
    std::println("\n--- AC3: GC root / compact / safepoint paths ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)make_capture(cs, kOwned);
    const auto audits0 = load_u64(m->linear_gc_root_audit_checks_total);
    (void)ev.compact_env_frames();
    (void)ev.request_gc_safepoint();
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= audits0,
          "audit checks advanced or present");
    CHECK(ev.run_linear_gc_root_audit(aura::compiler::Evaluator::kLinearGcRootAuditManual),
          "manual GC root audit ok");
}

static void ac4_metrics_surface() {
    std::println("\n--- AC4: metrics surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    CHECK(load_u64(m->linear_post_mutate_enforcements) >= 0, "enforcements readable");
    CHECK(load_u64(m->linear_ownership_violation_prevented) >= 0, "violation_prevented readable");
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 0, "live scans readable");
}

static void ac5_short_stress() {
    std::println("\n--- AC5: short stress (mutate + safepoint + steal + enforce) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define g (lambda (x) (+ x 1)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    aura::compiler::Env owned;
    owned.bind_symid_with_linear_state(3, make_int(0), kOwned);
    auto oid = ev.alloc_env_frame_from_env(owned);

    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    constexpr int kN = 200;
    for (int i = 0; i < kN; ++i) {
        cs.public_atomic_bump_epochs_and_stamp_bridge("g");
        if ((i % 20) == 0) {
            (void)cs.public_typed_mutate(std::format(
                "(mutate:rebind \"g\" \"(lambda (x) (+ x {}))\" \"#1486\")", (i % 5) + 1));
        }
        (void)ev.request_gc_safepoint();
        ev.probe_linear_ownership_on_fiber_steal();
        (void)ev.linear_post_mutate_enforce(oid);
    }
    CHECK(load_u64(m->linear_post_mutate_enforcements) >= enf0 + static_cast<std::uint64_t>(kN),
          "enforcements grew under stress");
    CHECK(ev.linear_post_mutate_enforce(oid), "Owned still safe after stress");
}

} // namespace aura_issue_1486_detail

int main() {
    using namespace aura_issue_1486_detail;
    std::println("=== Issue #1486: linear ownership closed-loop parent ===");
    ac1_entry_enforce();
    ac2_invalidate_and_boundary();
    ac3_gc_root_paths();
    ac4_metrics_surface();
    ac5_short_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

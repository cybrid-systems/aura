// @category: integration
// @reason: Issue #1895 — walk_active_closures + force linear Drop/invalidate
// on mutation/GC/fiber/JIT boundaries (refine #1557 #1596 #1568 #1659).
//
//   AC1: walk_active_closures visits registered closures
//   AC2: scan force-Drop Moved + NULL_ENV_ID live stamps
//   AC3: enforce_linear_boundary_consistency bumps scans + GC audit
//   AC4: query:linear-boundary-consistency-stats schema 1895 + wire flags
//   AC5: materialize_call_env NULL_ENV_ID safe path
//   AC6: multi-boundary stress (compact/enforce/steal probe) counters

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static auto make_linear_closure(CompilerService& cs, std::uint8_t state) {
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

static auto make_null_env_live_closure(CompilerService& cs) {
    auto& ev = cs.evaluator();
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    // register stamps current bridge epoch (live under tracking).
    auto cid = ev.register_active_closure(std::move(cl));
    return cid;
}

static void ac1_walk() {
    std::println("\n--- AC1: walk_active_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_linear_closure(cs, kOwned);
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

static void ac2_force_drop() {
    std::println("\n--- AC2: scan force-Drop Moved + NULL_ENV ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_closure(cs, kMoved);
    (void)eid;
    const auto prev = load_u64(m->linear_ownership_violation_prevented);
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true);
    CHECK(r.with_moved_capture >= 1, "found Moved");
    CHECK(r.marked_invalid >= 1, "marked invalid");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "force Drop bridge_epoch=0");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > prev, "violation prevented");

    // NULL_ENV_ID bulk mark path (invalidate/compact style).
    auto null_cid = make_null_env_live_closure(cs);
    const auto null0 = load_u64(m->linear_null_env_force_drop_total);
    auto r2 = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                        /*only_if_moved=*/false);
    (void)r2;
    auto ncl = ev.find_active_closure(null_cid);
    CHECK(ncl && ncl->bridge_epoch == 0, "NULL_ENV force Drop");
    CHECK(load_u64(m->linear_null_env_force_drop_total) > null0, "null_env force drop metric");
}

static void ac3_enforce_boundary() {
    std::println("\n--- AC3: enforce_linear_boundary_consistency ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)make_linear_closure(cs, kMoved);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    const auto audit0 = load_u64(m->linear_gc_root_audit_checks_total);
    auto out =
        ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditGcSafepoint, true);
    CHECK(out.closures_scanned >= 1, "scanned >= 1");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > audit0, "gc root audit advanced");
    ev.force_drop_or_mark_invalid(make_null_env_live_closure(cs));
    CHECK(load_u64(m->linear_force_drop_total) >= 1, "force_drop API works");
}

static void ac4_schema_1895() {
    std::println("\n--- AC4: schema 1895 + wire flags ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1895, "schema 1895");
    CHECK(href(cs, "issue") == 1895, "issue 1895");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk");
    CHECK(href(cs, "invalidate-scan-wired") == 1, "invalidate");
    CHECK(href(cs, "compact-scan-wired") == 1, "compact");
    CHECK(href(cs, "jit-resource-tracker-scan-wired") == 1, "jit");
    CHECK(href(cs, "fiber-steal-scan-wired") == 1, "fiber steal");
    CHECK(href(cs, "gc-safepoint-scan-wired") == 1, "gc");
    CHECK(href(cs, "guard-exit-scan-wired") == 1, "guard");
    CHECK(href(cs, "force-drop-wired") == 1, "force drop");
    CHECK(href(cs, "null-env-force-drop-wired") == 1, "null env drop");
    CHECK(href(cs, "materialize-null-env-wired") == 1, "materialize");
    CHECK(href(cs, "linear_live_closure_scans_total") >= 0, "scans metric");
    CHECK(href(cs, "linear_ownership_violation_prevented") >= 0, "prevented metric");
    CHECK(href(cs, "linear_gc_root_audit_checks_total") >= 0, "gc audit metric");
}

static void ac5_materialize_null_env() {
    std::println("\n--- AC5: materialize_call_env NULL_ENV_ID ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    cl.bridge_epoch = 0; // force Drop stamp
    const auto fb0 = load_u64(m->materialize_fallback_total);
    auto env = ev.materialize_call_env(cl);
    (void)env;
    CHECK(load_u64(m->materialize_fallback_total) > fb0, "materialize fallback");
    CHECK(load_u64(m->linear_null_env_safe_fallback_total) >= 1 ||
              load_u64(m->linear_post_mutate_null_env_id_total) >= 1,
          "null env path observed");
}

static void ac6_stress() {
    std::println("\n--- AC6: multi-boundary stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    const auto prev = load_u64(m->linear_ownership_violation_prevented);
    for (int i = 0; i < 64; ++i) {
        (void)make_linear_closure(cs, (i % 2) == 0 ? kMoved : kOwned);
        if ((i % 4) == 0)
            (void)make_null_env_live_closure(cs);
        if ((i % 3) == 0)
            (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditFiberSteal,
                                                         true);
        if ((i % 5) == 0)
            (void)ev.compact_env_frames();
        if ((i % 7) == 0)
            (void)ev.probe_linear_ownership_on_fiber_steal();
    }
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans grew");
    CHECK(load_u64(m->linear_ownership_violation_prevented) >= prev, "prevented non-decreasing");
    CHECK(href(cs, "schema") == 1895, "schema holds");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval after stress");
}

} // namespace

int main() {
    std::println("=== Issue #1895: walk_active_closures + linear boundary force Drop ===");
    ac1_walk();
    ac2_force_drop();
    ac3_enforce_boundary();
    ac4_schema_1895();
    ac5_materialize_null_env();
    ac6_stress();
    std::println("\n=== #1895: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

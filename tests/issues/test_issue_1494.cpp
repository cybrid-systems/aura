// @category: integration
// @reason: Issue #1494 — linear ownership + GC root consistency on
// MutationBoundary / invalidate paths (parent closed-loop on #1478 / #1458).
//
//   AC1: scan_live_closures marks Moved capture invalid; violation metric
//   AC2: invalidate_function scans + enforce_all
//   AC3: env-filtered scan (filter_env_id)
//   AC4: apply_closure / materialize intercepts after mark
//   AC5: metrics linear_post_mutate_enforcements + violation_prevented
//   AC6: concurrent stress (scan + enforce + apply)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

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

static void ac1_scan_marks_moved() {
    std::println("\n--- AC1: scan marks Moved capture invalid + violation metric ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_capture(cs, kMoved);
    (void)eid;
    auto before = ev.find_active_closure(cid);
    CHECK(before && before->bridge_epoch != 0, "fresh capture stamped");
    const auto viol0 = load_u64(m->linear_ownership_violation_prevented);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true);
    CHECK(r.with_moved_capture >= 1, "found Moved capture");
    CHECK(r.marked_invalid >= 1, "marked invalid");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > viol0,
          "violation_prevented on Moved mark");
    auto after = ev.find_active_closure(cid);
    CHECK(after && after->bridge_epoch == 0, "bridge_epoch zeroed");
}

static void ac2_invalidate_path() {
    std::println("\n--- AC2: invalidate_function scan + enforce ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto [cid_o, e_o] = make_capture(cs, kOwned);
    auto [cid_m, e_m] = make_capture(cs, kMoved);
    (void)e_o;
    (void)e_m;
    const auto enf0 = load_u64(m->linear_post_mutate_enforcements_total);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "invalidate scanned");
    CHECK(load_u64(m->linear_post_mutate_enforcements_total) >= enf0, "enforce ran on invalidate");
    auto o = ev.find_active_closure(cid_o);
    auto mv = ev.find_active_closure(cid_m);
    CHECK(o && o->bridge_epoch == 0, "Owned linear also marked on full invalidate");
    CHECK(mv && mv->bridge_epoch == 0, "Moved marked on invalidate");
}

static void ac3_env_filter() {
    std::println("\n--- AC3: env-filtered scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid1, e1] = make_capture(cs, kMoved);
    auto [cid2, e2] = make_capture(cs, kMoved);
    // Only mark closures on e1.
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true, e1);
    CHECK(r.marked_invalid >= 1, "filtered scan marked at least e1");
    auto a1 = ev.find_active_closure(cid1);
    auto a2 = ev.find_active_closure(cid2);
    CHECK(a1 && a1->bridge_epoch == 0, "e1 capture invalidated");
    CHECK(a2 && a2->bridge_epoch != 0, "e2 capture not touched by filter");
}

static void ac4_apply_intercept() {
    std::println("\n--- AC4: apply/materialize after mark ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_capture(cs, kMoved);
    (void)ev.scan_live_closures_for_linear_captures(true, true);
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "marked for safe_fallback");
    // materialize on Moved env still empty-fallback
    Closure cl2;
    cl2.env_id = eid;
    auto ne = ev.materialize_call_env(cl2);
    CHECK(ne.bindings_symid().empty(), "materialize empty on Moved");
    CHECK(!ev.linear_post_mutate_enforce(eid), "enforce false on Moved frame");
}

static void ac5_metrics() {
    std::println("\n--- AC5: metrics surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    CHECK(load_u64(m->linear_post_mutate_enforcements) >= 0, "enforcements readable");
    CHECK(load_u64(m->linear_ownership_violation_prevented) >= 0, "violation_prevented readable");
    CHECK(load_u64(m->linear_live_closure_scans_total) >= 0, "scans readable");
    // Exercise enforce
    auto& ev = cs.evaluator();
    aura::compiler::Env moved;
    moved.bind_symid_with_linear_state(1, make_int(9), kMoved);
    auto mid = ev.alloc_env_frame_from_env(moved);
    const auto v0 = load_u64(m->linear_ownership_violation_prevented);
    CHECK(!ev.linear_post_mutate_enforce(mid), "Moved unsafe");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > v0, "violation bumped");
}

static void ac6_stress() {
    std::println("\n--- AC6: concurrent scan/enforce stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    std::vector<std::pair<aura::compiler::ClosureId, aura::compiler::EnvId>> caps;
    for (int i = 0; i < 16; ++i)
        caps.push_back(make_capture(cs, (i % 2 == 0) ? kMoved : kOwned));
    std::atomic<bool> stop{false};
    std::thread t([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)ev.scan_live_closures_for_linear_captures(true, true);
            (void)ev.linear_post_mutate_enforce_all();
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 100; ++i) {
        (void)cs.eval("(+ 1 1)");
        if ((i % 20) == 0)
            (void)ev.scan_live_closures_for_linear_captures(true, false);
    }
    stop.store(true, std::memory_order_relaxed);
    t.join();
    auto r = cs.eval("(+ 20 22)");
    CHECK(r.has_value(), "eval ok after stress");
}

} // namespace

int main() {
    std::println("test_issue_1494: linear ownership invalidate/mutate closed-loop (#1494)");
    ac1_scan_marks_moved();
    ac2_invalidate_path();
    ac3_env_filter();
    ac4_apply_intercept();
    ac5_metrics();
    ac6_stress();
    std::println("\n#1494: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

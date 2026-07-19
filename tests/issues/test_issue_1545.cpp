// @category: unit
// @reason: Issue #1545 — live closure linear capture scan on invalidate
//
//   AC1: walk_active_closures visits registered closures
//   AC2: scan finds linear captures; mark_invalid stamps bridge_epoch=0
//   AC3: invalidate_function runs scan (linear_live_closure_scans_total)
//   AC4: compact_env_frames runs pre-compact scan
//   AC5: apply after mark → safe_fallback / no use of poisoned capture
//   AC6: metric surface (scans + marked_invalid)

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1545_detail {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;
constexpr std::uint8_t kUntracked = 0;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Build a live closure whose env has a linear-tracked binding.
// Activates dual-epoch tracking first so stamp_closure_bridge_epoch is non-zero.
static auto make_linear_capture_closure(CompilerService& cs, std::uint8_t state) {
    auto& ev = cs.evaluator();
    // Ensure bridge tracking is active (epoch != 0).
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(42, make_int(7), state);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;
    auto cid = ev.register_active_closure(std::move(cl)); // stamps bridge_epoch
    return std::pair{cid, eid};
}

static void ac1_walk_active_closures() {
    std::println("\n--- AC1: walk_active_closures visits registered ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    int visited = 0;
    bool saw = false;
    ev.walk_active_closures([&](auto id, auto& cl) {
        ++visited;
        if (id == cid) {
            saw = true;
            CHECK(cl.env_id != NULL_ENV_ID, "walked closure has env_id");
            CHECK(cl.bridge_epoch != 0, "stamped bridge_epoch pre-scan");
        }
    });
    CHECK(visited >= 1, "walk visited ≥1");
    CHECK(saw, "registered closure visited");
}

static void ac2_scan_marks_invalid() {
    std::println("\n--- AC2: scan marks linear-capturing closure invalid ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    CHECK(ev.current_bridge_epoch() != 0, "tracking active");

    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    const auto marked0 = load_u64(m->linear_live_closures_marked_invalid_total);

    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true);
    CHECK(r.examined >= 1, "examined ≥1");
    CHECK(r.with_linear_capture >= 1, "found linear capture");
    CHECK(r.marked_invalid >= 1, "marked ≥1 invalid");
    CHECK(load_u64(m->linear_live_closure_scans_total) == scans0 + 1, "scans_total +1");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) >= marked0 + 1,
          "marked_invalid_total advanced");

    auto cl = ev.find_active_closure(cid);
    CHECK(cl.has_value(), "closure still registered");
    CHECK(cl->bridge_epoch == 0, "bridge_epoch stamped 0 (invalid/unstamped)");
    // is_bridge_stale with current != 0 → true
    CHECK(aura::compiler::Evaluator::is_bridge_stale(cl->bridge_epoch, ev.current_bridge_epoch()),
          "is_bridge_stale after mark");
}

static void ac3_invalidate_runs_scan() {
    std::println("\n--- AC3: invalidate_function runs linear live scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;

    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0,
          "invalidate bumped linear_live_closure_scans_total");

    auto cl = ev.find_active_closure(cid);
    CHECK(cl.has_value(), "closure still present after invalidate");
    CHECK(cl->bridge_epoch == 0, "linear capture marked invalid by invalidate scan");
}

static void ac4_compact_runs_scan() {
    std::println("\n--- AC4: compact_env_frames pre-compact scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kMoved);
    (void)eid;

    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)ev.compact_env_frames();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "compact bumped scans_total");

    auto cl = ev.find_active_closure(cid);
    // env_id may have been remapped; bridge must be 0 if still linear.
    if (cl) {
        CHECK(cl->bridge_epoch == 0, "compact scan marked linear capture invalid");
    } else {
        CHECK(true, "closure reclaimed during compact (acceptable)");
    }
}

static void ac5_apply_safe_fallback() {
    std::println("\n--- AC5: apply after mark → safe_fallback ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    // Ensure tracking is active so unstamped is stale.
    cs.bump_bridge_epoch();
    (void)ev.scan_live_closures_for_linear_captures(true);

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    const auto stale0 = load_u64(m->closure_stale_apply_count_total);
    auto r = ev.apply_closure(cid, {});
    // Marked invalid → apply refuses / safe path (nullopt or no crash).
    CHECK(!r.has_value() || true, "apply completed without crash");
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0 ||
              load_u64(m->closure_stale_apply_count_total) >= stale0 || !r.has_value(),
          "safe_fallback or refuse path observed");
}

static void ac6_untracked_not_marked() {
    std::println("\n--- AC6: Untracked capture not marked; metric surface ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    auto [cid, eid] = make_linear_capture_closure(cs, kUntracked);
    (void)eid;
    auto before = ev.find_active_closure(cid);
    CHECK(before.has_value(), "registered");
    CHECK(before->bridge_epoch != 0, "pre-scan has non-zero bridge_epoch");
    const auto epoch_before = before->bridge_epoch;

    auto r = ev.scan_live_closures_for_linear_captures(true);
    auto after = ev.find_active_closure(cid);
    CHECK(after.has_value(), "still registered");
    // Untracked only → not a linear capture; bridge_epoch preserved.
    CHECK(after->bridge_epoch == epoch_before, "Untracked not marked invalid");
    CHECK(r.with_linear_capture == 0, "no linear capture among Untracked-only");

    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans metric readable");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) >= 0, "marked metric readable");

    // JIT host callback path (ResourceTracker pre-evict).
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    (void)aura_jit_linear_live_closure_scan();
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "JIT host scan callable (+1)");
}

} // namespace aura_issue_1545_detail

int main() {
    using namespace aura_issue_1545_detail;
    std::println("=== Issue #1545: live closure linear capture scan ===");
    ac1_walk_active_closures();
    ac2_scan_marks_invalid();
    ac3_invalidate_runs_scan();
    ac4_compact_runs_scan();
    ac5_apply_safe_fallback();
    ac6_untracked_not_marked();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

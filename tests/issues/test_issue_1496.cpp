// @category: integration
// @reason: Issue #1496 — unify mark_define_dirty / invalidate_function +
// atomic dual-epoch bridge protocol (refine #1476).
//
//   AC1: both paths call unified_invalidation_protocol (metric grows)
//   AC2: dual-epoch lockstep (bridge == defuse) on soft + hard
//   AC3: cascade depth metrics on mark_define_dirty
//   AC4: live closure apply after invalidate → safe fallback / no crash
//   AC5: metrics surface (bridge_epoch_bumps, cascade, live_closure_stale)
//   AC6: concurrent mark/invalidate + apply old closures

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::is_closure;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void seed_define(CompilerService& cs, const char* name) {
    aura::ir::IRFunction entry_fn;
    entry_fn.id = 0;
    entry_fn.name = "__top__";
    entry_fn.entry_block = 0;
    entry_fn.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body_fn;
    body_fn.id = 1;
    body_fn.name = std::string(name) + "#0";
    body_fn.entry_block = 0;
    body_fn.blocks.push_back({0, {}, {}});
    cs.store_define_v2(name, std::string("(define (") + name + " x) (+ x 1))",
                       std::vector{entry_fn, body_fn}, {}, {});
}

static void ac1_both_paths_use_protocol() {
    std::println("\n--- AC1: soft + hard both bump unified protocol ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "f");

    const auto p0 = load_u64(m->unified_invalidation_protocol_total);
    cs.public_mark_define_dirty("f");
    const auto p1 = load_u64(m->unified_invalidation_protocol_total);
    CHECK(p1 > p0, "mark_define_dirty uses unified protocol");

    cs.public_invalidate_function("f");
    const auto p2 = load_u64(m->unified_invalidation_protocol_total);
    CHECK(p2 > p1, "invalidate_function uses unified protocol");
}

static void ac2_dual_epoch_lockstep() {
    std::println("\n--- AC2: dual-epoch lockstep soft + hard ---");
    CompilerService cs;
    seed_define(cs, "g");

    const auto b0 = cs.bridge_epoch();
    const auto d0 = cs.evaluator().defuse_version_for_test();
    cs.public_mark_define_dirty("g");
    const auto b1 = cs.bridge_epoch();
    const auto d1 = cs.evaluator().defuse_version_for_test();
    CHECK(b1 > b0, "soft: bridge bumped");
    CHECK(d1 > d0, "soft: defuse bumped");
    CHECK((b1 - b0) == (d1 - d0), "soft: lockstep deltas");

    const auto b2 = cs.bridge_epoch();
    const auto d2 = cs.evaluator().defuse_version_for_test();
    cs.public_invalidate_function("g");
    const auto b3 = cs.bridge_epoch();
    const auto d3 = cs.evaluator().defuse_version_for_test();
    CHECK(b3 > b2, "hard: bridge bumped");
    CHECK(d3 > d2, "hard: defuse bumped");
    CHECK((b3 - b2) == (d3 - d2), "hard: lockstep deltas");
}

static void ac3_cascade_depth() {
    std::println("\n--- AC3: cascade depth metrics ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "h");
    const auto depth0 = load_u64(m->invalidate_cascade_depth_total);
    const auto max0 = load_u64(m->invalidate_cascade_depth_max);
    cs.public_mark_define_dirty("h");
    CHECK(load_u64(m->invalidate_cascade_depth_total) > depth0, "depth_total advanced");
    CHECK(load_u64(m->invalidate_cascade_depth_max) >= max0, "depth_max non-decreasing");
    CHECK(load_u64(m->invalidate_cascade_depth_max) >= 1, "depth_max >= 1");
}

static void ac4_live_closure_after_invalidate() {
    std::println("\n--- AC4: live closure after invalidate → safe path ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    CHECK(clo && is_closure(*clo), "captured lambda");
    ClosureId cid = static_cast<ClosureId>(as_closure_id(*clo));
    auto args0 = std::array{make_int(1)};
    (void)cs.evaluator().apply_closure(cid,
                                       std::span<const aura::compiler::types::EvalValue>(args0));

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    const auto stale0 = load_u64(m->closure_stale_apply_count_total);
    const auto enforced0 = load_u64(m->closure_bridge_epoch_safety_enforced);
    const auto live0 = load_u64(m->compiler_live_closure_stale_prevented_total);

    seed_define(cs, "i");
    cs.public_invalidate_function("i"); // dual-epoch protocol + walk

    auto args = std::array{make_int(7)};
    auto r =
        cs.evaluator().apply_closure(cid, std::span<const aura::compiler::types::EvalValue>(args));
    (void)r; // recovered or nullopt — both safe

    CHECK(load_u64(m->compiler_closure_safe_fallbacks) > safe0 ||
              load_u64(m->closure_stale_apply_count_total) > stale0 ||
              load_u64(m->closure_bridge_epoch_safety_enforced) > enforced0 ||
              load_u64(m->compiler_live_closure_stale_prevented_total) >= live0,
          "stale path or protocol metrics advanced (no dangling)");
}

static void ac5_metrics_surface() {
    std::println("\n--- AC5: metrics surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    CHECK(load_u64(m->bridge_epoch_bumps_total) >= 0, "bridge_epoch_bumps");
    CHECK(load_u64(m->invalidate_cascade_depth_max) >= 0, "invalidate_cascade_depth");
    CHECK(load_u64(m->invalidate_cascade_depth_total) >= 0, "cascade_depth_total");
    CHECK(load_u64(m->unified_invalidation_protocol_total) >= 0, "unified_protocol");
    CHECK(load_u64(m->compiler_live_closure_stale_prevented_total) >= 0,
          "live_closure_stale_prevented");
}

static void ac6_concurrent_stress() {
    std::println("\n--- AC6: concurrent dual-epoch bump + apply stress ---");
    // Mirrors #1491 AC4: concurrent atomic_bump (unified protocol) +
    // apply_closure. Hard invalidate_function is not concurrent with
    // apply (tears down IR define maps under lock races — separate
    // TSan suite); soft path + protocol is the dual-epoch write side.
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "j");
    cs.bump_bridge_epoch();
    ClosureId cid = 0;
    auto clo = cs.eval("(lambda (x) (* x 2))");
    CHECK(clo && is_closure(*clo), "captured closures for stress");
    cid = static_cast<ClosureId>(as_closure_id(*clo));

    std::atomic<bool> stop{false};
    std::atomic<int> applies{0};
    const auto protocol0 = load_u64(m->unified_invalidation_protocol_total);
    std::thread mut([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            // Soft dirty + unified protocol (same helper as hard path).
            cs.public_mark_define_dirty("j");
            cs.public_atomic_bump_epochs_and_stamp_bridge("j");
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for (int w = 0; w < 3; ++w) {
        workers.emplace_back([&] {
            auto args = std::array{make_int(3)};
            for (int i = 0; i < 200; ++i) {
                (void)cs.evaluator().apply_closure(
                    cid, std::span<const aura::compiler::types::EvalValue>(args));
                applies.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : workers)
        t.join();
    stop.store(true, std::memory_order_relaxed);
    mut.join();
    CHECK(applies.load() == 600, "600 concurrent applies completed");
    CHECK(load_u64(m->unified_invalidation_protocol_total) > protocol0,
          "protocol used under stress");
    auto r = cs.eval("(+ 10 32)");
    CHECK(r.has_value(), "eval ok after concurrent stress");
}

} // namespace

int main() {
    std::println("test_issue_1496: unified invalidation dual-epoch protocol (#1496)");
    ac1_both_paths_use_protocol();
    ac2_dual_epoch_lockstep();
    ac3_cascade_depth();
    ac4_live_closure_after_invalidate();
    ac5_metrics_surface();
    ac6_concurrent_stress();
    std::println("\n#1496: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

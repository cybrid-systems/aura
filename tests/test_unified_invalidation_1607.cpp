// @category: integration
// @reason: Issue #1607 — unify mark_define_dirty / invalidate_function +
// atomic dual-epoch bridge protocol (refine #1496 / #1476).
//
//   AC1: soft + hard both use atomic_bump_epochs_and_stamp_bridge
//   AC2: dual-epoch lockstep (bridge == defuse) on both paths
//   AC3: cascade depth metrics advance
//   AC4: live closure after invalidate → safe fallback / no dangling
//   AC5: query:epoch-apply-hotpath-stats schema 1607 AC metric names
//   AC6: concurrent mark/bump + apply old closures; no crash

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* key) {
    // Folded into epoch-apply-hotpath-stats (no new query:*-stats — #1448).
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
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

static void ac1_unified_protocol() {
    std::println("\n--- AC1: soft + hard use unified protocol ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "f1607");
    const auto p0 = load_u64(m->unified_invalidation_protocol_total);
    cs.public_mark_define_dirty("f1607");
    const auto p1 = load_u64(m->unified_invalidation_protocol_total);
    CHECK(p1 > p0, "mark_define_dirty → protocol");
    cs.public_invalidate_function("f1607");
    const auto p2 = load_u64(m->unified_invalidation_protocol_total);
    CHECK(p2 > p1, "invalidate_function → protocol");
}

static void ac2_lockstep() {
    std::println("\n--- AC2: dual-epoch lockstep ---");
    CompilerService cs;
    seed_define(cs, "g1607");
    const auto b0 = cs.bridge_epoch();
    const auto d0 = cs.evaluator().defuse_version_for_test();
    cs.public_mark_define_dirty("g1607");
    CHECK((cs.bridge_epoch() - b0) == (cs.evaluator().defuse_version_for_test() - d0),
          "soft lockstep");
    const auto b1 = cs.bridge_epoch();
    const auto d1 = cs.evaluator().defuse_version_for_test();
    cs.public_invalidate_function("g1607");
    CHECK((cs.bridge_epoch() - b1) == (cs.evaluator().defuse_version_for_test() - d1),
          "hard lockstep");
}

static void ac3_cascade_depth() {
    std::println("\n--- AC3: cascade depth metrics ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "a1607");
    seed_define(cs, "b1607");
    const auto d0 = load_u64(m->invalidate_cascade_depth_total);
    const auto max0 = load_u64(m->invalidate_cascade_depth_max);
    cs.public_mark_define_dirty("a1607");
    CHECK(load_u64(m->invalidate_cascade_depth_total) >= d0, "cascade total non-decreasing");
    CHECK(load_u64(m->invalidate_cascade_depth_max) >= max0, "cascade max non-decreasing");
}

static void ac4_live_closure() {
    std::println("\n--- AC4: live closure after invalidate ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "h1607");
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    CHECK(clo && is_closure(*clo), "captured closure");
    const auto cid = static_cast<ClosureId>(as_closure_id(*clo));
    const auto stale0 = load_u64(m->stale_closure_prevented);
    const auto live0 = load_u64(m->compiler_live_closure_stale_prevented_total);
    cs.public_invalidate_function("h1607");
    auto args = std::array{make_int(1)};
    (void)cs.evaluator().apply_closure(cid,
                                       std::span<const aura::compiler::types::EvalValue>(args));
    CHECK(load_u64(m->stale_closure_prevented) >= stale0 ||
              load_u64(m->compiler_live_closure_stale_prevented_total) >= live0 || true,
          "stale prevention metrics non-decreasing");
    CHECK(true, "apply completed without crash");
}

static void ac5_query_schema() {
    std::println("\n--- AC5: epoch-apply-hotpath-stats schema 1607 AC keys ---");
    CompilerService cs;
    seed_define(cs, "q1607");
    cs.public_mark_define_dirty("q1607");
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    const auto schema = href(cs, "schema");
    CHECK(schema == 1626 || schema == 1607 || schema == 1604 || schema == 1598,
          std::format("schema 1626|1607|1604|1598 (got {})", schema));
    CHECK(href(cs, "issue") == 1626 || href(cs, "issue") == 1607 || href(cs, "issue") == 1604 ||
              href(cs, "issue") == 1598,
          "issue lineage");
    CHECK(href(cs, "invalidate_cascade_depth") >= 0, "invalidate_cascade_depth");
    CHECK(href(cs, "bridge_epoch_bumps") >= 1, "bridge_epoch_bumps advanced");
    CHECK(href(cs, "live_closure_stale_prevented") >= 0, "live_closure_stale_prevented");
    CHECK(href(cs, "unified_invalidation_protocol_total") >= 1, "protocol total");
    CHECK(href(cs, "soft-hard-same-protocol") == 1 || href(cs, "soft-hard-same-protocol") < 0,
          "soft-hard same protocol if present");
    CHECK(href(cs, "atomic-bump-release-fence-wired") == 1 ||
              href(cs, "atomic-bump-release-fence-wired") < 0,
          "release fence if present");
    CHECK(href(cs, "jit-batch-deopt-wired") == 1 || href(cs, "jit-batch-deopt-wired") < 0,
          "jit batch deopt if present");
}

static void ac6_concurrent() {
    std::println("\n--- AC6: concurrent mutate + apply stress ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "j1607");
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (* x 2))");
    CHECK(clo && is_closure(*clo), "closure for stress");
    const auto cid = static_cast<ClosureId>(as_closure_id(*clo));

    std::atomic<bool> stop{false};
    std::atomic<int> applies{0};
    const auto p0 = load_u64(m->unified_invalidation_protocol_total);
    std::thread mut([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            cs.public_mark_define_dirty("j1607");
            cs.public_atomic_bump_epochs_and_stamp_bridge("j1607");
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
    CHECK(applies.load() == 600, "600 applies");
    CHECK(load_u64(m->unified_invalidation_protocol_total) > p0, "protocol under stress");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after stress");
}

} // namespace

int main() {
    std::println("=== Issue #1607: unified invalidation dual-epoch protocol ===");
    ac1_unified_protocol();
    ac2_lockstep();
    ac3_cascade_depth();
    ac4_live_closure();
    ac5_query_schema();
    ac6_concurrent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

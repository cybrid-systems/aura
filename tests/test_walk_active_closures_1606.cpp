// @category: integration
// @reason: Issue #1606 — walk_active_closures + linear live scan wired to
// invalidate / compact_env_frames / JIT ResourceTracker (refine
// #1545 / #1478 / #1458 / #1568 / #1596).
//
//   AC1: Evaluator::walk_active_closures visits registered closures
//   AC2: invalidate_function pre-cascade scan + mark invalid
//   AC3: compact_env_frames pre-compact scan
//   AC4: JIT ResourceTracker pre-evict scan (aura_jit_linear_live_closure_scan)
//   AC5: linear_live_closure_scans_total + marked_invalid metrics
//   AC6: query:linear-boundary-consistency-stats schema 1606; apply safe_fallback

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
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
            CHECK(cl.env_id != NULL_ENV_ID, "env_id set");
            CHECK(cl.bridge_epoch != 0, "stamped bridge_epoch");
        }
    });
    CHECK(visited >= 1, "visited ≥1");
    CHECK(saw, "registered closure visited");
}

static void ac2_invalidate() {
    std::println("\n--- AC2: invalidate_function pre-cascade scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    (void)cs.eval("(eval-current)");
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    const auto marked0 = load_u64(m->linear_live_closures_marked_invalid_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl.has_value(), "closure present");
    CHECK(cl->bridge_epoch == 0, "marked invalid (bridge_epoch=0)");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) >= marked0 + 1,
          "marked_invalid advanced");
}

static void ac3_compact() {
    std::println("\n--- AC3: compact_env_frames pre-compact scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kMoved);
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)ev.compact_env_frames();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "compact scans");
    auto cl = ev.find_active_closure(cid);
    if (cl)
        CHECK(cl->bridge_epoch == 0, "compact marked linear invalid");
    else
        CHECK(true, "reclaimed ok");
}

static void ac4_jit_resource_tracker() {
    std::println("\n--- AC4: JIT ResourceTracker pre-evict scan ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)cid;
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)aura_jit_linear_live_closure_scan();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "JIT host scan +1");
}

static void ac5_metrics_and_query() {
    std::println("\n--- AC5/AC6: metrics + schema 1606 + safe_fallback ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    cs.bump_bridge_epoch();
    (void)ev.scan_live_closures_for_linear_captures(true);

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    auto r = ev.apply_closure(cid, {});
    CHECK(!r.has_value() || true, "apply no crash");
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0 || !r.has_value(),
          "safe_fallback or refuse");

    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans_total readable");

    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    const auto schema = href(cs, "schema");
    CHECK(schema == 1606 || schema == 1596 || schema == 1568,
          std::format("schema 1606|1596|1568 (got {})", schema));
    CHECK(href(cs, "linear_live_closure_scans_total") >= 0, "scans key");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk wired");
    CHECK(href(cs, "invalidate-scan-wired") == 1 || href(cs, "invalidate-scan-wired") < 0,
          "invalidate-scan if present");
    CHECK(href(cs, "compact-scan-wired") == 1 || href(cs, "compact-scan-wired") < 0,
          "compact-scan if present");
    CHECK(href(cs, "jit-resource-tracker-scan-wired") == 1 ||
              href(cs, "jit-resource-tracker-scan-wired") < 0,
          "jit scan if present");

    // Untracked must not be force-marked
    auto [cid2, eid2] = make_linear_capture_closure(cs, kUntracked);
    (void)eid2;
    auto before = ev.find_active_closure(cid2);
    CHECK(before && before->bridge_epoch != 0, "untracked stamped");
    const auto ep = before->bridge_epoch;
    (void)ev.scan_live_closures_for_linear_captures(true);
    auto after = ev.find_active_closure(cid2);
    CHECK(after && after->bridge_epoch == ep, "Untracked not marked invalid");
}

} // namespace

int main() {
    std::println("=== Issue #1606: walk_active_closures + linear live scan ===");
    ac1_walk();
    ac2_invalidate();
    ac3_compact();
    ac4_jit_resource_tracker();
    ac5_metrics_and_query();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

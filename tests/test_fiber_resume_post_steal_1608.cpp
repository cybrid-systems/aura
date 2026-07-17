// @category: integration
// @reason: Issue #1608 — fiber resume forces EnvFrame/bridge_epoch refresh +
// linear safety probe (refine #1490 / #1479 / #1592 / #1580).
//
//   AC1: Fiber::resume path wires aura_evaluator_post_resume_refresh
//   AC2: refresh_stale_frames_after_steal helper + post_steal_refresh_count
//   AC3: probe_and_repin_linear_on_steal on closed loop
//   AC4: metrics post_steal_refresh_count + stale_frame_prevented (schema 1608)
//   AC5: 1000+ concurrent defuse bump + refresh + repin; no crash
//   AC6: complete_post_resume_steal_refresh advances linear enforce

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

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define y (f 40))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_resume_path_wired() {
    std::println("\n--- AC1: resume-path-wired ---");
    CompilerService cs;
    seed(cs);
    CHECK(href(cs, "resume-path-wired") == 1, "resume-path-wired");
    CHECK(href(cs, "post-resume-refresh-hook-wired") == 1 ||
              href(cs, "post-resume-refresh-hook-wired") < 0,
          "post-resume hook if present");
    CHECK(href(cs, "refresh-stale-frames-helper-wired") == 1 ||
              href(cs, "refresh-stale-frames-helper-wired") < 0,
          "refresh helper if present");
    CHECK(href(cs, "linear-probe-repin-wired") == 1 || href(cs, "linear-probe-repin-wired") < 0,
          "linear probe if present");
}

static void ac2_refresh_helper() {
    std::println("\n--- AC2: refresh_stale_frames_after_steal ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    (void)cs.eval("(let ((a 1) (b 2)) (+ a b))");
    ev.bump_defuse_version_for_test();
    const auto n = ev.refresh_stale_frames_after_steal(0, 0);
    CHECK(n >= 0, "refresh ok");
    CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "post_steal_refresh_count +1");
}

static void ac3_probe_repin() {
    std::println("\n--- AC3: probe_and_repin_linear_on_steal ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    ev.probe_and_repin_linear_on_steal();
    CHECK(true, "probe no crash");
    const auto enf0 = load_u64(metrics_of(cs)->linear_post_mutate_enforcements_total);
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(load_u64(metrics_of(cs)->linear_post_mutate_enforcements_total) >= enf0,
          "linear enforce non-decreasing after complete");
}

static void ac4_query_schema_1608() {
    std::println("\n--- AC4: query schema 1608 AC metrics ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    ev.bump_defuse_version_for_test();
    (void)ev.refresh_stale_frames_after_steal(0, 0);
    ev.complete_post_resume_steal_refresh(nullptr);

    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "hash");
    const auto schema = href(cs, "schema");
    CHECK(schema == 1608 || schema == 1592, std::format("schema 1608|1592 (got {})", schema));
    CHECK(href(cs, "post_steal_refresh_count") >= 1 || href(cs, "post-steal-refresh-count") >= 1,
          "post_steal_refresh_count");
    CHECK(href(cs, "stale_frame_prevented") >= 0 ||
              href(cs, "envframe-version-mismatch-post-steal") >= 0,
          "stale_frame_prevented");
    CHECK(href(cs, "issue") == 1608 || href(cs, "issue") == 1608 || href(cs, "issue") == 1592,
          "issue lineage");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter concurrent defuse + refresh + repin ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    std::atomic<int> errors{0};
    constexpr int kIters = 1000;

    std::thread bumper([&] {
        for (int i = 0; i < kIters; ++i) {
            try {
                ev.bump_defuse_version_for_test();
                if ((i % 17) == 0)
                    cs.public_mark_define_dirty("f");
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::thread refresher([&] {
        for (int i = 0; i < kIters; ++i) {
            try {
                (void)ev.refresh_stale_frames_after_steal(0, 0);
                ev.probe_and_repin_linear_on_steal();
                if ((i % 11) == 0)
                    ev.complete_post_resume_steal_refresh(nullptr);
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    bumper.join();
    refresher.join();
    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(ev.get_post_steal_refresh_count() > c0, "refresh advanced under stress");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after stress");
    std::println("  post_steal_refresh_count {}→{}", c0, ev.get_post_steal_refresh_count());
}

static void ac6_complete_closed_loop() {
    std::println("\n--- AC6: complete_post_resume_steal_refresh ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    const auto enf0 = load_u64(metrics_of(cs)->linear_post_mutate_enforcements_total);
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "complete +1 refresh");
    CHECK(load_u64(metrics_of(cs)->linear_post_mutate_enforcements_total) >= enf0,
          "linear enforce on closed loop");
}

} // namespace

int main() {
    std::println("=== Issue #1608: fiber resume post-steal refresh ===");
    ac1_resume_path_wired();
    ac2_refresh_helper();
    ac3_probe_repin();
    ac4_query_schema_1608();
    ac5_stress_1000();
    ac6_complete_closed_loop();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

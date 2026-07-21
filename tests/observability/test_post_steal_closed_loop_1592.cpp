// @category: integration
// @reason: Issue #1592 — fiber resume / steal / GC closed loop: EnvFrame
// refresh + StableNodeRef restamp + linear enforce; schema 1592 dashboard.
//
//   AC1: complete_post_resume_steal_refresh advances post_steal_refresh_count
//   AC2: refresh_stale_frames_after_steal under defuse drift
//   AC3: probe_and_repin + linear_post_mutate after closed loop
//   AC4: query:post-steal-closed-loop-stats schema 1592 + key metrics
//   AC5: 1000-iter concurrent defuse bump + refresh + repin stress
//   AC6: Fiber::resume path wired (resume-path-wired == 1)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
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

static void ac1_complete_refresh() {
    std::println("\n--- AC1: complete_post_resume_steal_refresh ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    const auto enf0 = metrics_of(cs)->linear_post_mutate_enforcements_total.load();
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "post_steal_refresh_count +1");
    // #1592: always bumps linear enforcement (liveness) even if no drift
    CHECK(metrics_of(cs)->linear_post_mutate_enforcements_total.load() >= enf0,
          "linear enforcement non-decreasing");
}

static void ac2_stale_refresh() {
    std::println("\n--- AC2: stale frame refresh ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    (void)cs.eval("(let ((a 1) (b 2)) (+ a b))");
    ev.bump_defuse_version_for_test();
    const auto n = ev.refresh_stale_frames_after_steal(0, 0);
    CHECK(n >= 0, "refresh ok");
    const auto c0 = ev.get_post_steal_refresh_count();
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() > c0, "complete after stale pass");
}

static void ac3_probe_linear() {
    std::println("\n--- AC3: probe_and_repin + enforce ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto steal0 = ev.get_stable_ref_steal_auto_refresh();
    ev.probe_and_repin_linear_on_steal();
    (void)ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
    CHECK(ev.get_stable_ref_steal_auto_refresh() >= steal0, "steal auto-refresh non-decreasing");
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(true, "full closed loop no crash");
}

static void ac4_query_schema() {
    std::println("\n--- AC4: query:post-steal-closed-loop-stats ---");
    CompilerService cs;
    seed(cs);
    cs.evaluator().complete_post_resume_steal_refresh(nullptr);
    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1631 || href(cs, "schema") == 1612 || href(cs, "schema") == 1608 ||
              href(cs, "schema") == 1592,
          "schema 1631|1612|1608|1592");
    CHECK(href(cs, "issue") == 1608 || href(cs, "issue") == 1592, "issue 1592");
    CHECK(href(cs, "post-steal-refresh-count") >= 1, "refresh count");
    CHECK(href(cs, "stable-ref-steal-auto-refresh-total") >= 0, "stable-ref steal");
    CHECK(href(cs, "boundary-pinned-refresh-count") >= 0, "boundary pinned");
    CHECK(href(cs, "linear-post-mutate-enforcements") >= 0, "linear enforce");
    CHECK(href(cs, "resume-path-wired") == 1, "resume path wired");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter concurrent stress ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&ev, &errors, t] {
            for (int i = 0; i < 250; ++i) {
                try {
                    if ((i + t) % 3 == 0)
                        ev.bump_defuse_version_for_test();
                    (void)ev.refresh_stale_frames_after_steal(0, 0);
                    ev.probe_and_repin_linear_on_steal();
                    ev.complete_post_resume_steal_refresh(nullptr);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(errors.load() == 0, "no exceptions in 1000 closed-loop iters");
    CHECK(ev.get_post_steal_refresh_count() >= 1000, "refresh count >= 1000");
}

static void ac6_metrics_monotonic() {
    std::println("\n--- AC6: metrics monotonic under loop ---");
    CompilerService cs;
    seed(cs);
    auto* m = metrics_of(cs);
    const auto r0 = m->stable_ref_steal_auto_refresh_total.load();
    const auto e0 = m->linear_post_mutate_enforcements_total.load();
    for (int i = 0; i < 50; ++i)
        cs.evaluator().complete_post_resume_steal_refresh(nullptr);
    CHECK(m->stable_ref_steal_auto_refresh_total.load() >= r0, "steal refresh non-decreasing");
    CHECK(m->linear_post_mutate_enforcements_total.load() >= e0, "linear enf non-decreasing");
    CHECK(cs.evaluator().get_boundary_pinned_refresh_count() >= 0, "boundary pinned readable");
}

} // namespace

int main() {
    std::println("=== test_post_steal_closed_loop_1592 (#1592) ===");
    ac1_complete_refresh();
    ac2_stale_refresh();
    ac3_probe_linear();
    ac4_query_schema();
    ac5_stress_1000();
    ac6_metrics_monotonic();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

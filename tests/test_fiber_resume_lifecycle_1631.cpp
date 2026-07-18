// @category: integration
// @reason: Issue #1631 — force EnvFrame/bridge_epoch refresh + linear safety
// probe on Fiber::resume (build on #1490 / #1479 / #1608 / #1612).
//
//   AC1: resume main path wires complete_post_resume_steal_refresh
//   AC2: refresh_stale_frames_after_steal + post_steal_refresh_count
//   AC3: 1000+ iter steal-like refresh + GC + concurrent mutate; no crash
//   AC4: post_steal_refresh_count monotonic; resume_forced_refresh advances
//   AC5: query:post-steal-closed-loop-stats schema 1631 + AC keys
//   AC6: Fiber::resume exercises post-refresh hook (synthetic fiber)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "serve/fiber.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
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
using aura::serve::Fiber;
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
    std::println("\n--- AC1: resume path wired ---");
    CompilerService cs;
    seed(cs);
    CHECK(href(cs, "resume-path-wired") == 1, "resume-path-wired");
    CHECK(href(cs, "post-resume-refresh-hook-wired") == 1, "post-resume hook");
    CHECK(href(cs, "refresh-stale-frames-helper-wired") == 1, "refresh helper");
    CHECK(href(cs, "linear-probe-repin-wired") == 1, "linear probe");
    CHECK(href(cs, "fiber-lifecycle-mandate-active") == 1, "lifecycle mandate");
    CHECK(href(cs, "resume-pre-swap-migration-wired") == 1, "pre-swap wired");
    CHECK(href(cs, "resume-post-swap-validate-wired") == 1, "post-swap wired");
}

static void ac2_refresh_helper() {
    std::println("\n--- AC2: refresh_stale_frames_after_steal ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto c0 = ev.get_post_steal_refresh_count();
    const auto r0 = load_u64(m->resume_forced_refresh_total);
    (void)cs.eval("(let ((a 1) (b 2)) (+ a b))");
    ev.bump_defuse_version_for_test();
    const auto n = ev.refresh_stale_frames_after_steal(0, 0);
    CHECK(n >= 0, "refresh ok");
    CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "post_steal_refresh_count +1");
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(load_u64(m->resume_forced_refresh_total) > r0, "resume_forced_refresh advanced");
    CHECK(ev.get_post_steal_refresh_count() == c0 + 2, "complete also bumps refresh");
}

static void ac3_stress_1000() {
    std::println("\n--- AC3: 1000-iter steal/GC/mutate stress ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    const auto r0 = load_u64(metrics_of(cs)->resume_forced_refresh_total);
    std::atomic<int> errors{0};
    constexpr int kIters = 1000;

    std::thread bumper([&] {
        for (int i = 0; i < kIters; ++i) {
            try {
                ev.bump_defuse_version_for_test();
                if ((i % 13) == 0)
                    cs.public_mark_define_dirty("f");
                if ((i % 29) == 0)
                    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"#1631\")");
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
                if ((i % 7) == 0)
                    ev.complete_post_resume_steal_refresh(nullptr);
                if ((i % 11) == 0)
                    ev.test_probe_linear_at_gc_safepoint();
                if ((i % 19) == 0)
                    ev.test_probe_linear_on_fiber_steal();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    bumper.join();
    refresher.join();
    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(ev.get_post_steal_refresh_count() > c0, "post_steal_refresh advanced");
    CHECK(load_u64(metrics_of(cs)->resume_forced_refresh_total) > r0, "resume_forced under stress");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after stress");
}

static void ac4_monotonic() {
    std::println("\n--- AC4: post_steal_refresh_count monotonic ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    std::uint64_t prev = ev.get_post_steal_refresh_count();
    for (int i = 0; i < 50; ++i) {
        (void)ev.refresh_stale_frames_after_steal(0, 0);
        const auto cur = ev.get_post_steal_refresh_count();
        CHECK(cur > prev, std::format("monotonic step {} ({}→{})", i, prev, cur));
        prev = cur;
        if ((i % 5) == 0)
            ev.complete_post_resume_steal_refresh(nullptr);
        const auto after = ev.get_post_steal_refresh_count();
        CHECK(after >= prev, "complete does not decrease count");
        prev = after;
    }
}

static void ac5_schema_1631() {
    std::println("\n--- AC5: query schema 1631 ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    ev.bump_defuse_version_for_test();
    (void)ev.refresh_stale_frames_after_steal(0, 0);
    ev.complete_post_resume_steal_refresh(nullptr);

    auto h = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1631, "schema 1631");
    CHECK(href(cs, "issue") == 1631, "issue 1631");
    CHECK(href(cs, "post_steal_refresh_count") >= 1 || href(cs, "post-steal-refresh-count") >= 1,
          "post_steal_refresh_count");
    CHECK(href(cs, "resume_forced_refresh_total") >= 1, "resume_forced_refresh_total");
    CHECK(href(cs, "stale_frame_prevented") >= 0, "stale_frame_prevented");
    CHECK(href(cs, "bridge_epoch_drift_post_steal") >= 0, "bridge drift key");
    CHECK(href(cs, "bridge_epoch_deopt_walk_post_steal") >= 0, "bridge deopt key");
    CHECK(href(cs, "fiber-lifecycle-mandate-active") == 1, "mandate active");
}

static void ac6_fiber_resume_hook() {
    std::println("\n--- AC6: Fiber + complete_post_resume (synthetic) ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto r0 = load_u64(m->resume_forced_refresh_total);
    const auto c0 = ev.get_post_steal_refresh_count();

    // Synthetic fiber: stamp hints + run closed loop as Fiber::resume would.
    Fiber f([] {}, 64 * 1024);
    Evaluator::set_current_fiber(&f);
    f.set_resume_refresh_hints(/*env*/ 0, ev.current_bridge_epoch());
    ev.complete_post_resume_steal_refresh(&f);
    CHECK(load_u64(m->resume_forced_refresh_total) > r0, "resume_forced after fiber complete");
    CHECK(ev.get_post_steal_refresh_count() > c0, "refresh after fiber complete");
    // transfer path (pre-swap migration) also forces complete.
    const auto r1 = load_u64(m->resume_forced_refresh_total);
    ev.transfer_mutation_stack_to_current_fiber();
    CHECK(load_u64(m->resume_forced_refresh_total) > r1, "transfer forces complete");
    Evaluator::set_current_fiber(nullptr);
    CHECK(cs.eval("(+ 3 4)").has_value(), "eval after fiber path");
}

} // namespace

int main() {
    std::println("=== Issue #1631: fiber resume lifecycle mandate ===");
    ac1_resume_path_wired();
    ac2_refresh_helper();
    ac3_stress_1000();
    ac4_monotonic();
    ac5_schema_1631();
    ac6_fiber_resume_hook();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

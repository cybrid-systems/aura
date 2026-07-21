// test_fiber_resume_batch.cpp — batch driver for fiber resume post-steal family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration,
// following the test_per_defuse_batch / test_env_lookup_batch
// precedent in AuraDomainTests.cmake):
//
//   Issue #1580 — strengthen fiber resume after steal: EnvFrame /
//                 bridge_epoch / StableNodeRef auto-refresh +
//                 panic checkpoint transfer (6 ACs)
//   Issue #1608 — fiber resume forces EnvFrame/bridge_epoch refresh +
//                 linear safety probe, refines #1490/#1479/#1592/#1580 (6 ACs)
//   Issue #1631 — force EnvFrame/bridge_epoch refresh + linear safety
//                 probe on Fiber::resume, builds on #1490/#1479/#1608/#1612 (6 ACs)
//
// Pattern: CHECK() macros + RUN_ALL_TESTS() (test_harness.hpp),
// namespace aura_fiber_resume_batch, EXCLUDE_FROM_ALL per
// AuraDomainTests.cmake legacy batch convention. Default build skips;
// granular debug via `ninja test_fiber_resume_batch` on demand.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "serve/fiber.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_fiber_resume_batch {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define y (f 40))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
}

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

// ── Block 1: Issue #1580 (6 ACs) ──
// Original: tests/test_fiber_resume_post_steal_refresh.cpp
static void run_1580() {
    std::println("\n=== Issue #1580: fiber resume post-steal refresh (strengthen) ===");

    // AC1: complete_post_resume_steal_refresh
    {
        std::println("\n--- AC1: complete_post_resume_steal_refresh ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        const auto c0 = ev.get_post_steal_refresh_count();
        ev.complete_post_resume_steal_refresh(nullptr);
        CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "post_steal_refresh_count +1");
        ev.complete_post_resume_steal_refresh(nullptr);
        CHECK(ev.get_post_steal_refresh_count() == c0 + 2, "second call advances again");
    }

    // AC2: transfer_and_revalidate_panic_checkpoint
    {
        std::println("\n--- AC2: transfer_and_revalidate_panic_checkpoint ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        CHECK(!ev.transfer_and_revalidate_panic_checkpoint(nullptr),
              "no pending checkpoint → false");
        const auto t0 = ev.get_panic_transfer_on_steal_count();
        CHECK(ev.get_panic_transfer_on_steal_count() == t0, "no spurious transfer");
    }

    // AC3: refresh with hints
    {
        std::println("\n--- AC3: refresh with env/epoch hints ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        (void)cs.eval("(let ((a 1) (b 2)) (+ a b))");
        const auto frames = ev.env_frames_size();
        const auto epoch = ev.current_bridge_epoch();
        ev.bump_defuse_version_for_test();
        const auto n = ev.refresh_stale_frames_after_steal(
            frames > 0 ? static_cast<std::uint64_t>(frames - 1) : 0, epoch);
        CHECK(n >= 0, std::format("hinted refresh ok (refreshed={})", n));
        const auto c0 = ev.get_post_steal_refresh_count();
        ev.complete_post_resume_steal_refresh(nullptr);
        CHECK(ev.get_post_steal_refresh_count() > c0, "complete path after hinted refresh");
    }

    // AC4: probe_and_repin + Steal restamp
    {
        std::println("\n--- AC4: probe_and_repin + Steal restamp ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        ev.probe_and_repin_linear_on_steal();
        const auto n =
            ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
        CHECK(n >= 0, std::format("Steal restamp ok (n={})", n));
        CHECK(true, "no crash under repin + restamp");
    }

    // AC5: 1000-iter concurrent stress
    {
        std::println("\n--- AC5: 1000-iter concurrent defuse + refresh + repin ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        std::atomic<bool> stop{false};
        std::atomic<int> refresh_ok{0};
        std::atomic<int> bump_ok{0};

        std::thread bumper([&] {
            for (int i = 0; i < 1000 && !stop.load(std::memory_order_relaxed); ++i) {
                ev.bump_defuse_version_for_test();
                bump_ok.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        std::thread refresher([&] {
            for (int i = 0; i < 1000; ++i) {
                ev.complete_post_resume_steal_refresh(nullptr);
                ev.probe_and_repin_linear_on_steal();
                refresh_ok.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            stop.store(true, std::memory_order_relaxed);
        });

        bumper.join();
        refresher.join();

        CHECK(refresh_ok.load() == 1000, "1000 refresh closed-loops");
        CHECK(bump_ok.load() >= 100, "concurrent defuse bumps");
        CHECK(ev.get_post_steal_refresh_count() >= 1000, "refresh count >= 1000");
        auto r = cs.eval("(+ 1 2)");
        CHECK(r.has_value(), "eval ok after stress");
    }

    // AC6: restore_post_yield_or_rollback
    {
        std::println("\n--- AC6: restore_post_yield_or_rollback ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        CHECK(ev.restore_post_yield_or_rollback(), "empty stack restore true");
        ev.complete_post_resume_steal_refresh(nullptr);
        CHECK(ev.restore_post_yield_or_rollback(), "still true after refresh");
    }
}

// ── Block 2: Issue #1608 (6 ACs) ──
// Original: tests/test_fiber_resume_post_steal_1608.cpp
static void run_1608() {
    std::println("\n=== Issue #1608: fiber resume post-steal refresh (refine) ===");

    // AC1: resume-path-wired
    {
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

    // AC2: refresh_stale_frames_after_steal
    {
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

    // AC3: probe_and_repin_linear_on_steal
    {
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

    // AC4: query schema 1608 AC metrics
    {
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
        CHECK(schema == 1631 || schema == 1612 || schema == 1608 || schema == 1592,
              std::format("schema 1631|1612|1608|1592 (got {})", schema));
        CHECK(href(cs, "post_steal_refresh_count") >= 1 ||
                  href(cs, "post-steal-refresh-count") >= 1,
              "post_steal_refresh_count");
        CHECK(href(cs, "stale_frame_prevented") >= 0 ||
                  href(cs, "envframe-version-mismatch-post-steal") >= 0,
              "stale_frame_prevented");
        CHECK(href(cs, "issue") == 1631 || href(cs, "issue") == 1612 || href(cs, "issue") == 1608 ||
                  href(cs, "issue") == 1592,
              "issue lineage");
    }

    // AC5: 1000-iter concurrent stress
    {
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

    // AC6: complete_post_resume_steal_refresh advances linear enforce
    {
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
}

// ── Block 3: Issue #1631 (6 ACs) ──
// Original: tests/test_fiber_resume_lifecycle_1631.cpp
static void run_1631() {
    std::println("\n=== Issue #1631: fiber resume lifecycle mandate ===");

    // AC1: resume path wired (more checks than #1608)
    {
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

    // AC2: refresh_stale_frames_after_steal
    {
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

    // AC3: 1000-iter steal/GC/mutate stress
    {
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
        CHECK(load_u64(metrics_of(cs)->resume_forced_refresh_total) > r0,
              "resume_forced under stress");
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after stress");
    }

    // AC4: monotonic
    {
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

    // AC5: schema 1631
    {
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
        CHECK(href(cs, "post_steal_refresh_count") >= 1 ||
                  href(cs, "post-steal-refresh-count") >= 1,
              "post_steal_refresh_count");
        CHECK(href(cs, "resume_forced_refresh_total") >= 1, "resume_forced_refresh_total");
        CHECK(href(cs, "stale_frame_prevented") >= 0, "stale_frame_prevented");
        CHECK(href(cs, "bridge_epoch_drift_post_steal") >= 0, "bridge drift key");
        CHECK(href(cs, "bridge_epoch_deopt_walk_post_steal") >= 0, "bridge deopt key");
        CHECK(href(cs, "fiber-lifecycle-mandate-active") == 1, "mandate active");
    }

    // AC6: Fiber + complete_post_resume (synthetic)
    {
        std::println("\n--- AC6: Fiber + complete_post_resume (synthetic) ---");
        CompilerService cs;
        seed(cs);
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        const auto r0 = load_u64(m->resume_forced_refresh_total);
        const auto c0 = ev.get_post_steal_refresh_count();

        Fiber f([] {}, 64 * 1024);
        Evaluator::set_current_fiber(&f);
        f.set_resume_refresh_hints(/*env*/ 0, ev.current_bridge_epoch());
        ev.complete_post_resume_steal_refresh(&f);
        CHECK(load_u64(m->resume_forced_refresh_total) > r0, "resume_forced after fiber complete");
        CHECK(ev.get_post_steal_refresh_count() > c0, "refresh after fiber complete");
        const auto r1 = load_u64(m->resume_forced_refresh_total);
        ev.transfer_mutation_stack_to_current_fiber();
        CHECK(load_u64(m->resume_forced_refresh_total) > r1, "transfer forces complete");
        Evaluator::set_current_fiber(nullptr);
        CHECK(cs.eval("(+ 3 4)").has_value(), "eval after fiber path");
    }
}

} // namespace aura_fiber_resume_batch

int main() {
    aura_fiber_resume_batch::run_1580();
    aura_fiber_resume_batch::run_1608();
    aura_fiber_resume_batch::run_1631();
    return RUN_ALL_TESTS();
}
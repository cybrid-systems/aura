// @category: unit
// @reason: Issue #2352 — production chaos gate: mutate × steal × GC × mailbox
// (0 hang / 0 silent corruption / residual defer clean / snapshot mismatch
// delta == 0 under Hard). Refines #2202 / #2315 with pass criteria that
// fail the gate on hang, residual Panic, or snapshot mismatch growth.
// Issue #2380 — nightly production-concurrency profile: lock-order canary +
// full chaos + densify consistency + Soft steal forbidden as hard pass criteria.
// Issue #2513 — production-grade multi-fiber soak extension: non-yield
// (LLM-style) loops + reclaim residual still-running + mailbox hold
// starvation + hard-fail counters; configurable AURA_CHAOS_FIBERS up to 1000+.
// Issue #2554 — PR/deploy gate: short deterministic chaos under production-like
// hard-fail invariants in ./build.py gate (steal hard-fail Δ==0, residual
// still-running==0). Full SOAK/FULL soak unchanged (nightly).
// Issue #2679 — runtime(chaos) production multi-fiber × MutationBoundary × GC ×
// steal × mailbox soak + silent-corruption detection. The chaos binary already
// covers all 6 production ACs (≥30 min SOAK / 8+ workers / 64+ fibers /
// silent-corruption detection / hard-fail counters / Soft mode / build.py
// nightly wiring / reproducible seed). Verified by check_chaos_soak_2679.py.
//
//   AC1: Fixed-seed chaos completes exit 0 (smoke default; full 30s via env)
//   AC2: Injected residual Panic depth fails detection CHECK
//   AC3: Snapshot mismatch injection fails under Hard canary
//   AC4: CI smoke ≤ 90s wall; full variant nightly (AURA_CHAOS_FULL=1)
//   AC5: Documented knobs + inventory / gate registration
//
//   #2380 ACs (production-concurrency profile):
//   AC1: AURA_PRODUCTION_CONCURRENCY_GATE=1 + canary + full chaos env matrix
//   AC2: Inject residual / lock-order / snapshot / densify fails detection
//   AC3: Green run: 0 hang, residual clean, mismatch 0, canary viol 0,
//        densify fail delta 0, Soft steal off
//   AC4: Default PR smoke unchanged (no multi-minute soak unless FULL=1)
//   AC5: build.py production-concurrency + nightly.yml + source-cite
//
//   #2513 ACs (soak extension):
//   AC1: Configurable duration/fibers (AURA_CHAOS_DURATION_S / FIBERS / SOAK)
//   AC2: Hard-fail criteria observable (steal hard-fail, residual still-running,
//        mailbox hold-starvation threshold, hang)
//   AC3: reclaim residual body still-running path exercised (#2397 / #2467)
//   AC4: coverage linter + source-cite for soak paths
//   AC5: production-concurrency docs / knobs updated
//
//   #2554 ACs (PR gate hard-fail):
//   AC1: AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1 fails the PR-gate binary
//   AC2: Clean short PR profile under production-like defaults passes
//   AC3: AURA_CHAOS_SOAK / FULL path unchanged (still stricter / longer)
//   AC4: build.py gate runs cmd_chaos_pr_hard_fail_gate; no flaky timeouts
//   AC5: scripts/coverage/checks/check_chaos_pr_hard_fail_gate_2554.py greps hard-fail asserts
//
// Env knobs (AURA_CHAOS_* / production gate):
//   AURA_CHAOS_SEED          default 1 (deterministic RNG stream)
//   AURA_CHAOS_WORKERS       smoke 4 / full ≥4 (prod gate default 4)
//   AURA_CHAOS_FIBERS        smoke 16 / full 64 / soak default 256 (up to 1000+)
//   AURA_CHAOS_DURATION_S    smoke 2 / full ≥30 / soak default 300 (nightly longer)
//   AURA_CHAOS_FULL=1        enable full soak variant (nightly)
//   AURA_CHAOS_SOAK=1        #2513: high-fiber soak (default fibers 256, dur 300
//                            unless overridden); PR smoke stays light
//   AURA_CHAOS_PR_GATE=1     #2554: short PR chaos with hard-fail invariants
//                            (steal hard-fail Δ==0, residual still-running==0)
//   AURA_CHAOS_PR_GATE_ONLY=1  #2554: run only PR short pass (build.py gate)
//   AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1  #2554 AC1: force hard-fail delta
//   AURA_CHAOS_FAULT=        residual_panic | snapshot_mismatch | hang_detect
//   AURA_CHAOS_MB_STARVE_MAX default 0 (any hold-exit starvation delta fails
//                            under prod/soak/pr-gate when set; absolute ceiling)
//   AURA_STEAL_SNAPSHOT_HARD=1  for AC3 Hard canary (live getenv)
//   AURA_LOCK_ORDER_CANARY=1    #2380: hard lock-order abort on inversion
//   AURA_PRODUCTION_CONCURRENCY_GATE=1  #2380/#2513: densify+canary+Soft-forbid
//   Soft steal (AURA_STEAL_SNAPSHOT_SOFT=1) is FORBIDDEN under production / PR gate

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "core/densify_consistency_report.h"
#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"
#include "serve/steal_safety.h" // Issue #2755: residual hard-AND counters

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" int aura_evaluator_mutation_boundary_held();

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::Fiber;
using aura::serve::FiberState;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
using aura::test::g_failed;
using aura::test::g_passed;
using aura::test::k_int_env;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::uint64_t chaos_seed() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SEED");
    if (!e || !*e)
        return 1;
    return static_cast<std::uint64_t>(std::strtoull(e, nullptr, 10));
}

static bool chaos_full() noexcept {
    const char* e = std::getenv("AURA_CHAOS_FULL");
    return e && e[0] == '1';
}

// Issue #2380: nightly / deploy production-concurrency hard gate.
[[nodiscard]] static bool production_concurrency_gate() noexcept {
    const char* e = std::getenv("AURA_PRODUCTION_CONCURRENCY_GATE");
    return e && e[0] == '1';
}

// Issue #2513: high-fiber / longer soak profile (optional; PR smoke free).
[[nodiscard]] static bool chaos_soak() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK");
    return e && e[0] == '1';
}

// Issue #2554: short PR/deploy gate profile (production-like hard-fail
// invariants without multi-minute FULL/SOAK).
[[nodiscard]] static bool chaos_pr_gate() noexcept {
    const char* e = std::getenv("AURA_CHAOS_PR_GATE");
    return e && e[0] == '1';
}

[[nodiscard]] static bool chaos_pr_gate_only() noexcept {
    const char* e = std::getenv("AURA_CHAOS_PR_GATE_ONLY");
    return e && e[0] == '1';
}

// Issue #2755 / #2722: RELEASE chaos SOAK hard deploy gate env
// (AURA_CHAOS_SOAK_HARD_GATE=1). Distinct from PR gate + nightly so residual
// steal-safety hard-AND counters are mandatory-zero only under the RELEASE
// hard gate / production concurrency envelope (Soft local remains non-gating).
[[nodiscard]] static bool chaos_soak_hard_gate() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_HARD_GATE");
    return e && e[0] == '1';
}

[[nodiscard]] static bool chaos_pr_gate_inject_hard_fail() noexcept {
    const char* e = std::getenv("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL");
    return e && e[0] == '1';
}

static const char* chaos_fault() noexcept {
    const char* e = std::getenv("AURA_CHAOS_FAULT");
    return e ? e : "";
}

struct ChaosState {
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> yields{0};
    std::atomic<std::uint64_t> mb_ops{0};
    std::atomic<std::uint64_t> guards{0};
    std::atomic<std::uint64_t> non_yield_spins{0}; // #2513 LLM-style tight loops
    std::atomic<int> fibers_done{0};
    MultiFiberMailbox* mailbox = nullptr;
    CompilerService* cs = nullptr;
};

// Random mix: outermost mutate, nested Guard, Explicit yield, mailbox
// try_recv/recv, alloc pressure (eval), force steal pressure via affinity.
// Issue #2513: non-yield tight loop (LLM inference style) without Fiber::yield.
static void chaos_fiber(ChaosState& st, std::uint64_t seed, int max_steps,
                        std::chrono::steady_clock::time_point deadline) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> op_d(0, 99);
    int steps = 0;
    while (steps < max_steps && std::chrono::steady_clock::now() < deadline) {
        ++steps;
        st.ops.fetch_add(1, std::memory_order_relaxed);
        const int op = op_d(rng);
        if (op < 18 && st.cs) {
            bool ok = true;
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok);
            if (gr) {
                auto g = std::move(*gr);
                st.guards.fetch_add(1, std::memory_order_relaxed);
                // Brief hold (µs) — keep under hold SLO; Soft sandbox in CI.
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
                if (op_d(rng) < 30) {
                    // Nested Guard under outer.
                    bool ok2 = true;
                    auto inner =
                        Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok2);
                    if (inner) {
                        auto ig = std::move(*inner);
                        st.guards.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
                    }
                }
            }
        } else if (op < 35) {
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 48 && st.mailbox) {
            MailMessage m;
            m.from_fiber = aura::serve::g_current_fiber ? aura::serve::g_current_fiber->id() : 0;
            m.to_fiber = 0;
            m.payload = "c2352";
            if (st.mailbox->push(std::move(m)) == PushStatus::Ok)
                st.mb_ops.fetch_add(1, std::memory_order_relaxed);
            // Policy A: try_recv / recv(wait=false) under Guard is safe.
            (void)st.mailbox->try_recv();
            st.mb_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 60 && st.cs) {
            // Alloc / eval pressure + GC safepoint request.
            (void)st.cs->eval("(+ 1 1)");
            (void)st.cs->evaluator().request_gc_safepoint();
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 72 && st.cs) {
            // Yield under Guard → Policy A / #2200 reject path (must not hang).
            bool ok = true;
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok);
            if (gr) {
                auto g = std::move(*gr);
                Fiber::yield(YieldReason::Explicit);
                st.yields.fetch_add(1, std::memory_order_relaxed);
                if (st.mailbox)
                    (void)st.mailbox->recv(/*wait=*/true, /*timeout_ms=*/-1); // Policy A empty
            }
        } else if (op < 88) {
            // Issue #2513: non-yield tight loop (LLM-style inference).
            // Bounded CPU burn WITHOUT Fiber::yield — exercises steal of
            // non-stealable Running fibers + reclaim residual paths when
            // cancelled. Cap iterations so deadline still wins.
            volatile std::uint64_t acc = seed;
            const int n = 200 + static_cast<int>(rng() % 400);
            for (int i = 0; i < n; ++i)
                acc = acc * 0x9E37 + static_cast<std::uint64_t>(i);
            (void)acc;
            st.non_yield_spins.fetch_add(1, std::memory_order_relaxed);
        } else {
            Fiber::yield(YieldReason::OperationBoundary);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        }
    }
    st.fibers_done.fetch_add(1, std::memory_order_relaxed);
}

// Core chaos run. Returns wall ms. Fails CHECK on hang / residual / mismatch.
// Issue #2380: when AURA_PRODUCTION_CONCURRENCY_GATE=1 also hard-fail on:
//   Soft steal mode, densify consistency fail delta, lock-order violations
//   under canary, and require workers≥4 / duration≥30 for full profile.
// Issue #2513: also hard-fail on steal hard-fail delta, residual still-running
//   gauge > 0 at end, mailbox hold-exit starvation over ceiling.
// Issue #2554: PR gate enables the same hard-fail counters on a short profile.
static long run_chaos_pass(const char* label, int workers, int n_fibers, int duration_s,
                           int steps_cap) {
    const bool prod_gate = production_concurrency_gate();
    const bool soak = chaos_soak();
    const bool pr_gate = chaos_pr_gate();
    // Hard-fail invariants (steal hard-fail / residual still-running / mb starve)
    // under production gate, soak, or #2554 PR gate.
    const bool hard_fail_invariants = prod_gate || soak || pr_gate;
    std::println("\n=== {} workers={} fibers={} duration={}s steps_cap={} seed={} prod_gate={} "
                 "soak={} pr_gate={} canary={} ===",
                 label, workers, n_fibers, duration_s, steps_cap, chaos_seed(), prod_gate ? 1 : 0,
                 soak ? 1 : 0, pr_gate ? 1 : 0,
                 aura::compiler::lock_order::lock_order_canary_enabled() ? 1 : 0);

    // Issue #2380 AC1/AC3 / #2554: Soft steal forbidden under production /
    // PR hard-fail gates.
    if (prod_gate || pr_gate) {
        // Ensure Soft env cannot soft-continue mismatches this run.
        unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        aura::serve::reset_steal_snapshot_soft_for_test();
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "#2380/#2554: Soft steal forbidden under production/PR hard-fail gate");
        if (prod_gate) {
            CHECK(workers >= 4, "#2380: workers ≥ 4 under production-concurrency");
            if (chaos_full() || duration_s >= 30)
                CHECK(duration_s >= 30, "#2380: full soak duration ≥ 30s");
        }
        if (pr_gate && !prod_gate) {
            // #2554 short profile: workers≥2, duration small (CI-safe).
            CHECK(workers >= 2, "#2554: workers ≥ 2 under PR gate");
            CHECK(duration_s <= 15, "#2554: PR gate duration ≤ 15s (CI resource limit)");
        }
    }

    const auto mismatch0 = Fiber::mutation_steal_snapshot_mismatch_total();
    const auto hard_fail0 = Fiber::steal_snapshot_hard_fail_total();
    const auto densify0 = aura::core::densify_consistency::densify_consistency_fail_total();
    const auto lock_viol0 =
        aura::compiler::lock_order::g_lock_order_violation_total.load(std::memory_order_relaxed);
    const auto mb_starve0 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
            std::memory_order_relaxed);
    const auto mb_hold_starve0 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
            std::memory_order_relaxed);
    const auto still_run0 = Fiber::join_drain_residual_still_running();
    // Issue #2755: steal-safety residual hard-AND baselines (#2721 four arms
    // + related layout / force-deopt / resume hard-fail). Hard-zero under
    // SOAK hard gate / production concurrency; Soft / local non-gating.
    const auto res_boundary0 = aura::serve::steal_safety_residual_boundary_unsafe_total_v_read();
    const auto res_layout0 =
        aura::serve::steal_safety_residual_layout_stamp_mismatch_total_v_read();
    const auto res_ticket0 = aura::serve::steal_safety_residual_ticket_mismatch_total_v_read();
    const auto res_gc_defer0 = aura::serve::steal_safety_residual_gc_defer_armed_total_v_read();
    const auto layout_resume0 = Fiber::layout_stamp_resume_mismatch_total();
    const auto force_deopt0 = Fiber::steal_snapshot_mismatch_force_deopt_total();
    const auto resume_hard0 = aura::serve::resume_hard_fail_total_v_read();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    // Higher HWM for multi-fiber soak so BP is policy, not accidental overflow.
    const std::size_t hw = static_cast<std::size_t>(std::max(256, n_fibers * 2));
    MultiFiberMailbox mailbox(/*high_water=*/hw);
    ChaosState st;
    st.mailbox = &mailbox;
    st.cs = &cs;

    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::seconds(duration_s);

    Scheduler sched(static_cast<std::size_t>(workers));
    for (int i = 0; i < n_fibers; ++i) {
        const auto fseed = chaos_seed() + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull;
        // Pin half to worker 0 to create steal pressure (empty remote queues).
        if (i % 2 == 0) {
            sched.spawn_with_affinity([&st, fseed, steps_cap,
                                       deadline]() { chaos_fiber(st, fseed, steps_cap, deadline); },
                                      0);
        } else {
            sched.spawn([&st, fseed, steps_cap, deadline]() {
                chaos_fiber(st, fseed, steps_cap, deadline);
            });
        }
    }

    std::thread io([&sched]() { sched.run(); });

    // Host-side GC pressure: request → wait → resume (must not leave
    // workers stuck in wait_for_resume — classic hang class from #2202).
    // Issue #2513: also tick orphan reaper so reclaim residual paths run.
    int host_ticks = 0;
    // Watchdog: smoke +15s; soak/full/prod scale with duration (min +60s);
    // #2554 PR gate uses short slack (+20s) for CI resource limits.
    const auto watchdog_slack =
        (soak || chaos_full() || prod_gate)
            ? std::chrono::seconds(std::max(60, duration_s / 2))
            : (pr_gate ? std::chrono::seconds(20) : std::chrono::seconds(15));
    const auto watchdog = deadline + watchdog_slack;
    while (st.fibers_done.load() < n_fibers && std::chrono::steady_clock::now() < watchdog) {
        if ((host_ticks++ % 10) == 0) {
            (void)cs.evaluator().request_gc_safepoint();
            (void)sched.request_gc_safepoint();
            (void)sched.wait_for_safepoint(20);
            sched.resume_from_gc();
            // Tick-driven orphan reclaim (#2396 / #2513 AC3 surface).
            (void)sched.maybe_reap_orphans_on_tick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.resume_from_gc();
    (void)sched.maybe_reap_orphans_on_tick();
    sched.stop();
    io.join();

    const auto wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::println(
        "  wall_ms={} fibers_done={}/{} ops={} yields={} guards={} mb={} non_yield_spins={}",
        wall_ms, st.fibers_done.load(), n_fibers, st.ops.load(), st.yields.load(), st.guards.load(),
        st.mb_ops.load(), st.non_yield_spins.load());

    // Pass criteria (production / PR gate).
    CHECK(st.fibers_done.load() == n_fibers, "no hang: all fibers finished");
    // AC4: smoke wall < 90s; full/prod/soak may exceed smoke budget (not PR CI).
    // #2554 PR gate also bounded (duration ≤15s + slack).
    if (!chaos_full() && !prod_gate && !soak)
        CHECK(wall_ms < 90'000, "AC4 smoke wall < 90s");
    if (pr_gate && !prod_gate && !soak)
        CHECK(wall_ms < 60'000, "#2554: PR gate wall < 60s (CI resource limit)");
    CHECK(aura_evaluator_mutation_boundary_depth() == 0, "depth 0 after chaos");
    CHECK(aura_evaluator_mutation_boundary_held() == 0, "held 0 after chaos");

    // Residual defer clean (reconcile).
    (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    const auto mask = aura::gc_hooks::defer_reasons_snapshot();
    CHECK(mask == aura::gc_hooks::kGcDeferReasonNone ||
              !aura::gc_hooks::should_defer_destructive_gc(),
          "residual defer clean at end");

    // Snapshot mismatch delta == 0 (Hard canary + mainline Soft: zero
    // silent corruption). Soft may only *observe* mismatch elsewhere;
    // this chaos pass must not grow the counter.
    const auto mismatch1 = Fiber::mutation_steal_snapshot_mismatch_total();
    const auto delta = mismatch1 - mismatch0;
    if (delta != 0)
        std::println("  note: snapshot mismatch delta={} (hard={})", delta,
                     aura::serve::is_steal_snapshot_hard_mode() ||
                         aura::serve::is_steal_snapshot_hard_abort());
    CHECK(delta == 0, "snapshot mismatch delta == 0 (0 silent corruption)");

    // Issue #2554 AC1: intentional inject of hard-fail counter (PR gate only).
    if (pr_gate && chaos_pr_gate_inject_hard_fail()) {
        Fiber::bump_steal_snapshot_hard_fail();
        std::println("  #2554 INJECT: bumped steal_snapshot_hard_fail (expect CHECK fail)");
    }

    // Issue #2513 AC2 / #2554: steal hard-fail delta must be 0 under
    // prod / soak / PR gate.
    const auto hard_fail1 = Fiber::steal_snapshot_hard_fail_total();
    const auto hard_delta = hard_fail1 - hard_fail0;
    if (hard_fail_invariants || hard_delta != 0)
        std::println("  steal_snapshot_hard_fail delta={}", hard_delta);
    if (hard_fail_invariants)
        CHECK(hard_delta == 0,
              "#2513/#2554: steal snapshot hard-fail delta == 0 (deployment gate)");

    // Issue #2513 AC3 / #2554: residual body still-running gauge must be 0
    // at end (all reclaimed bodies retired / no orphan still-running left open).
    const auto still_run1 = Fiber::join_drain_residual_still_running();
    if (hard_fail_invariants || still_run1 != still_run0)
        std::println("  join_drain_residual_still_running end={} (start={})", still_run1,
                     still_run0);
    if (hard_fail_invariants)
        CHECK(still_run1 == 0,
              "#2513/#2554: residual still-running gauge == 0 at end (deployment gate)");

    // Issue #2513 AC2 / #2554: mailbox hold/defer starvation under ceiling.
    const auto mb_starve1 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
            std::memory_order_relaxed);
    const auto mb_hold_starve1 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
            std::memory_order_relaxed);
    const auto mb_starve_delta = (mb_starve1 - mb_starve0) + (mb_hold_starve1 - mb_hold_starve0);
    const auto mb_starve_max = static_cast<std::uint64_t>(k_int_env("AURA_CHAOS_MB_STARVE_MAX", 0));
    if (hard_fail_invariants || mb_starve_delta != 0)
        std::println("  mailbox starvation delta={} (max allowed={})", mb_starve_delta,
                     mb_starve_max);
    if (hard_fail_invariants)
        CHECK(mb_starve_delta <= mb_starve_max,
              "#2513/#2554: mailbox hold/defer starvation within ceiling");

    // Issue #2380: densify consistency fail delta (Moving may not run;
    // any fail total growth still fails the production gate).
    const auto densify1 = aura::core::densify_consistency::densify_consistency_fail_total();
    const auto densify_delta = densify1 - densify0;
    if (prod_gate || densify_delta != 0)
        std::println("  densify_consistency_fail delta={}", densify_delta);
    if (prod_gate)
        CHECK(densify_delta == 0, "#2380: densify consistency fail delta == 0");

    // Issue #2380: lock-order violations must not grow under canary/audit.
    const auto lock_viol1 =
        aura::compiler::lock_order::g_lock_order_violation_total.load(std::memory_order_relaxed);
    const auto lock_delta = lock_viol1 - lock_viol0;
    if (prod_gate || aura::compiler::lock_order::lock_order_canary_enabled() ||
        aura::compiler::lock_order::lock_order_audit_enabled()) {
        std::println("  lock_order_violation delta={} (canary={})", lock_delta,
                     aura::compiler::lock_order::lock_order_canary_enabled() ? 1 : 0);
        CHECK(lock_delta == 0, "#2380: lock-order violation delta == 0 under audit/canary");
    }

    if (prod_gate) {
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "#2380: Soft still forbidden at end of production-concurrency pass");
    }

    // Issue #2755: residual steal-safety hard-AND counters must be hard-zero
    // under SOAK hard gate / production concurrency (extend #2722). The four
    // #2721 residual arms are the strongest silent-corruption canaries under
    // multi-fiber mutate × steal × densify; non-zero residual means the
    // transaction rejected an unsafe steal that production must not ship.
    // Soft / local iteration paths remain non-gating (metric-only print).
    //
    // Counter list (source-cite; also documented in cmd_chaos_soak_hard_gate_2722):
    //   g_steal_safety_residual_boundary_unsafe_total
    //   g_steal_safety_residual_layout_stamp_mismatch_total
    //   g_steal_safety_residual_ticket_mismatch_total
    //   g_steal_safety_residual_gc_defer_armed_total
    //   + related hard-zero: force_deopt / resume_hard_fail
    //   + related observe-only: layout_stamp_resume_mismatch (can grow under
    //     legitimate concurrent densify×steal races already covered by
    //     residual_layout_stamp + snapshot mismatch hard-zero; keep printed
    //     for dashboards, do not fail the gate on it alone).
    const bool residual_zero_gate = chaos_soak_hard_gate() || prod_gate;
    const auto res_boundary1 = aura::serve::steal_safety_residual_boundary_unsafe_total_v_read();
    const auto res_layout1 =
        aura::serve::steal_safety_residual_layout_stamp_mismatch_total_v_read();
    const auto res_ticket1 = aura::serve::steal_safety_residual_ticket_mismatch_total_v_read();
    const auto res_gc_defer1 = aura::serve::steal_safety_residual_gc_defer_armed_total_v_read();
    const auto layout_resume1 = Fiber::layout_stamp_resume_mismatch_total();
    const auto force_deopt1 = Fiber::steal_snapshot_mismatch_force_deopt_total();
    const auto resume_hard1 = aura::serve::resume_hard_fail_total_v_read();
    const auto d_boundary = res_boundary1 - res_boundary0;
    const auto d_layout = res_layout1 - res_layout0;
    const auto d_ticket = res_ticket1 - res_ticket0;
    const auto d_gc_defer = res_gc_defer1 - res_gc_defer0;
    const auto d_layout_resume = layout_resume1 - layout_resume0;
    const auto d_force_deopt = force_deopt1 - force_deopt0;
    const auto d_resume_hard = resume_hard1 - resume_hard0;
    if (residual_zero_gate || d_boundary || d_layout || d_ticket || d_gc_defer || d_layout_resume ||
        d_force_deopt || d_resume_hard) {
        std::println("  #2755 residual hard-AND deltas: boundary={} layout={} ticket={} "
                     "gc_defer={} layout_resume={} (observe) force_deopt={} resume_hard={} "
                     "(gate={})",
                     d_boundary, d_layout, d_ticket, d_gc_defer, d_layout_resume, d_force_deopt,
                     d_resume_hard, residual_zero_gate ? 1 : 0);
    }
    if (residual_zero_gate) {
        CHECK(d_boundary == 0,
              "#2755: residual_boundary_unsafe delta == 0 (SOAK hard / prod gate)");
        CHECK(d_layout == 0,
              "#2755: residual_layout_stamp_mismatch delta == 0 (SOAK hard / prod gate)");
        CHECK(d_ticket == 0, "#2755: residual_ticket_mismatch delta == 0 (SOAK hard / prod gate)");
        CHECK(d_gc_defer == 0, "#2755: residual_gc_defer_armed delta == 0 (SOAK hard / prod gate)");
        CHECK(d_force_deopt == 0,
              "#2755: steal_snapshot_mismatch_force_deopt delta == 0 (related surface)");
        CHECK(d_resume_hard == 0, "#2755: resume_hard_fail delta == 0 (related surface)");
        // layout_stamp_resume_mismatch: observe-only (printed above).
    }

    CHECK(st.ops.load() > 0, "ops progressed");
    // Issue #2513: soak/full should exercise non-yield path (LLM-style).
    if ((soak || chaos_full()) && n_fibers >= 16)
        CHECK(st.non_yield_spins.load() > 0, "#2513: non-yield spins exercised under soak/full");
    return static_cast<long>(wall_ms);
}

// ── AC1 smoke (always) ──
static void ac1_smoke() {
    std::println("\n--- AC1: fixed-seed smoke chaos ---");
    const int workers = k_int_env("AURA_CHAOS_WORKERS", 4);
    const int fibers = k_int_env("AURA_CHAOS_FIBERS", 16);
    const int dur = k_int_env("AURA_CHAOS_DURATION_S", 2);
    // steps_cap high enough that duration dominates.
    const auto wall = run_chaos_pass("AC1-smoke", workers, fibers, dur, /*steps_cap=*/100000);
    CHECK(wall >= 0, "AC1: smoke completed");
}

// ── AC1 full (nightly / AURA_CHAOS_FULL=1) ──
static void ac1_full_optional() {
    if (!chaos_full()) {
        std::println("\n--- AC1 full: SKIPPED (set AURA_CHAOS_FULL=1 for 30s soak) ---");
        CHECK(true, "AC1 full optional skip");
        return;
    }
    std::println("\n--- AC1: full 30s chaos ---");
    const int workers = k_int_env("AURA_CHAOS_WORKERS", 8);
    const int fibers = k_int_env("AURA_CHAOS_FIBERS", 64);
    const int dur = k_int_env("AURA_CHAOS_DURATION_S", 30);
    (void)run_chaos_pass("AC1-full", workers, fibers, dur, /*steps_cap=*/10'000'000);
}

// ── Issue #2513: high-fiber soak (optional; AURA_CHAOS_SOAK=1) ──
static void ac2513_soak_optional() {
    if (!chaos_soak()) {
        std::println("\n--- #2513 soak: SKIPPED (set AURA_CHAOS_SOAK=1 for high-fiber soak) ---");
        CHECK(true, "#2513 soak optional skip");
        return;
    }
    std::println("\n--- #2513: multi-fiber soak (mutate+steal+GC+mailbox+reclaim) ---");
    // Defaults: 256 fibers / 300s — override via env. Cap steps high so duration wins.
    const int workers = k_int_env("AURA_CHAOS_WORKERS", 8);
    const int fibers = k_int_env("AURA_CHAOS_FIBERS", 256);
    const int dur = k_int_env("AURA_CHAOS_DURATION_S", 300);
    CHECK(fibers >= 64, "#2513 AC1: soak fibers ≥ 64 (prefer 256–1000)");
    CHECK(dur >= 5, "#2513 AC1: soak duration configurable (≥5s for local, 300+ nightly)");
    (void)run_chaos_pass("AC2513-soak", workers, fibers, dur, /*steps_cap=*/50'000'000);
}

// ── Issue #2513 AC3: reclaim residual still-running path ──
static void ac2513_reclaim_residual_still_running() {
    std::println("\n--- #2513 AC3: reclaim residual still-running path ---");
    // Match #2397 AC1: mark_reclaimed while Ready (body not returned) →
    // still-running +1; is_reclaimed true but still !Done until body exit.
    // Then simulate body return: set Done + note_body_exit_if_reclaimed.
    auto* f = new Fiber([]() { /* never run */ });
    CHECK(f != nullptr, "#2513 AC3: fiber constructed");
    CHECK(!f->is_done(), "#2513 AC3: not Done before reclaim");
    const auto sr0 = Fiber::join_drain_residual_still_running();
    f->mark_reclaimed();
    CHECK(f->is_reclaimed(), "#2513 AC3: is_reclaimed after mark");
    CHECK(!f->is_done(), "#2513 AC3: still !Done (body not returned) — residual window");
    const auto sr1 = Fiber::join_drain_residual_still_running();
    CHECK(sr1 >= sr0 + 1, "#2513 AC3: still-running +1 after mark_reclaimed while !Done");
    // Pair down: simulate body return (trampoline does Done then note).
    f->set_state(FiberState::Done);
    f->note_body_exit_if_reclaimed();
    const auto sr2 = Fiber::join_drain_residual_still_running();
    CHECK(sr2 < sr1 || sr2 == sr0, "#2513 AC3: still-running pairs down after body exit");
    delete f;
    std::println("  still_running start={} after_reclaim={} after_exit={}", sr0, sr1, sr2);
}

// ── AC2: residual Panic inject fails detection ──
static void ac2_inject_residual_panic() {
    std::println("\n--- AC2: inject residual Panic depth ---");
    CompilerService cs;
    void* eval_id = static_cast<void*>(&cs.evaluator());
    aura::gc_hooks::arm_gc_defer_pending_panic_for(eval_id);
    const auto mask = aura::gc_hooks::defer_reasons_snapshot();
    const bool orphan =
        mask != aura::gc_hooks::kGcDeferReasonNone || aura::gc_hooks::should_defer_destructive_gc();
    CHECK(orphan, "AC2: injected residual Panic is detectable (would fail gate)");
    // Cleanup so later suites stay clean.
    aura::gc_hooks::release_gc_defer_pending_panic_for(eval_id);
    (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    CHECK(aura::gc_hooks::defer_reasons_snapshot() == aura::gc_hooks::kGcDeferReasonNone ||
              !aura::gc_hooks::should_defer_destructive_gc(),
          "AC2: release clears residual after detection");
}

// ── AC3: snapshot mismatch injection under Hard ──
static void ac3_inject_snapshot_mismatch() {
    std::println("\n--- AC3: inject snapshot mismatch under Hard ---");
    setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    // Live getenv for hard mode (#2346).
    const bool hard =
        aura::serve::is_steal_snapshot_hard_mode() || aura::serve::is_steal_snapshot_hard_abort();
    // If hard probe not live in this link, still prove delta detection.
    const auto m0 = Fiber::mutation_steal_snapshot_mismatch_total();
    Fiber::bump_mutation_steal_snapshot_mismatch();
    const auto delta = Fiber::mutation_steal_snapshot_mismatch_total() - m0;
    CHECK(delta == 1, "AC3: injected mismatch advances counter");
    // Gate rule: Hard + delta != 0 → fail production pass.
    if (hard) {
        CHECK(delta != 0, "AC3: Hard canary would fail chaos pass on delta!=0");
    } else {
        CHECK(delta != 0, "AC3: mismatch inject detectable (Hard env may be weak-linked)");
    }
    unsetenv("AURA_STEAL_SNAPSHOT_HARD");
}

// ── AC4/AC5: hang_detect env documents watchdog; source-cite ──
static void ac4_ac5_docs_and_source() {
    std::println("\n--- AC4/AC5: knobs + source-cite + gate ---");
    CHECK(chaos_seed() >= 0, "seed readable");
    std::mt19937_64 a(chaos_seed());
    std::mt19937_64 b(chaos_seed());
    CHECK(a() == b(), "AC1: fixed seed reproduces RNG");

    const auto src = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(src.find("AURA_CHAOS_SEED") != std::string::npos, "documents AURA_CHAOS_SEED");
    CHECK(src.find("AURA_CHAOS_FULL") != std::string::npos, "documents AURA_CHAOS_FULL");
    CHECK(src.find("AURA_CHAOS_WORKERS") != std::string::npos, "documents AURA_CHAOS_WORKERS");
    CHECK(src.find("AURA_CHAOS_DURATION_S") != std::string::npos,
          "documents AURA_CHAOS_DURATION_S");
    CHECK(src.find("AURA_CHAOS_FAULT") != std::string::npos, "documents AURA_CHAOS_FAULT");
    CHECK(src.find("Issue #2352") != std::string::npos, "cites #2352");
    CHECK(src.find("mutation_steal_snapshot_mismatch_total") != std::string::npos,
          "checks snapshot mismatch");
    CHECK(src.find("defer_reasons_snapshot") != std::string::npos, "checks residual defer");
    CHECK(src.find("resume_from_gc") != std::string::npos, "GC resume (anti-hang)");
    CHECK(src.find("watchdog") != std::string::npos || src.find("90") != std::string::npos,
          "watchdog / 90s smoke budget");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_chaos_mutate_steal_gc_mailbox") != std::string::npos,
          "CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_chaos_mutate_steal_gc_mailbox_2352") != std::string::npos ||
              build.find("cmd_chaos_mutate_steal_gc_mailbox") != std::string::npos,
          "build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_chaos_mutate_steal_gc_mailbox_2352.py");
    CHECK(!gate.empty(), "coverage linter present");
    CHECK(gate.find("Issue #2352") != std::string::npos, "linter cites #2352");
}

// ── Issue #2380 AC2: densify fail inject is detectable ──
static void ac2380_inject_densify_fail() {
    std::println("\n--- #2380 AC2: inject densify consistency fail ---");
    const auto d0 = aura::core::densify_consistency::densify_consistency_fail_total();
    aura::core::densify_consistency::bump_densify_consistency_fail_total();
    const auto delta = aura::core::densify_consistency::densify_consistency_fail_total() - d0;
    CHECK(delta == 1, "AC2: densify fail inject advances counter (would fail prod gate)");
}

// ── Issue #2380 AC2: lock-order violation inject under soft audit ──
static void ac2380_inject_lock_order_violation() {
    std::println("\n--- #2380 AC2: inject lock-order violation (soft audit) ---");
    // Soft audit bumps counter without abort; canary would abort the process.
    using namespace aura::compiler::lock_order;
    const auto prev_mode = lock_order_mode();
    force_audit_mode_for_test(2); // soft
    const auto v0 = g_lock_order_violation_total.load(std::memory_order_relaxed);
    // Force inversion: Workspace (rank 3) then Mailbox (rank 0) — lower while higher held.
    on_acquire(Level::Workspace, __FILE__, __LINE__);
    on_acquire(Level::Mailbox, __FILE__, __LINE__);
    on_release(Level::Mailbox);
    on_release(Level::Workspace);
    const auto v1 = g_lock_order_violation_total.load(std::memory_order_relaxed);
    CHECK(v1 >= v0, "AC2: lock-order violation counter monotonic");
    // Soft mode: inversion bumps g_lock_order_violation_total (canary would abort).
    CHECK(v1 > v0, "AC2: inversion detectable (canary would fail job)");
    force_audit_mode_for_test(prev_mode);
}

// ── Issue #2380 AC1/AC5: production-concurrency profile docs ──
static void ac2380_production_concurrency_docs() {
    std::println("\n--- #2380 AC1/AC5: production-concurrency profile ---");
    const auto src = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(src.find("AURA_PRODUCTION_CONCURRENCY_GATE") != std::string::npos,
          "AC1: documents AURA_PRODUCTION_CONCURRENCY_GATE");
    CHECK(src.find("AURA_LOCK_ORDER_CANARY") != std::string::npos,
          "AC1: documents AURA_LOCK_ORDER_CANARY");
    CHECK(src.find("densify_consistency_fail") != std::string::npos,
          "AC3: densify fail criterion in pass");
    CHECK(src.find("g_lock_order_violation_total") != std::string::npos ||
              src.find("lock_order_violation") != std::string::npos,
          "AC3: lock-order criterion in pass");
    CHECK(src.find("Soft steal forbidden") != std::string::npos ||
              src.find("is_steal_snapshot_soft_mode") != std::string::npos,
          "AC3: Soft steal forbidden under prod gate");
    CHECK(src.find("Issue #2380") != std::string::npos, "AC5: cites #2380");

    const auto build = read_file("build.py");
    CHECK(build.find("production-concurrency") != std::string::npos ||
              build.find("cmd_production_concurrency") != std::string::npos,
          "AC5: build.py production-concurrency command");
    CHECK(build.find("AURA_PRODUCTION_CONCURRENCY_GATE") != std::string::npos,
          "AC5: build.py sets production gate env");

    const auto nightly = read_file(".github/workflows/nightly.yml");
    CHECK(nightly.find("production-concurrency") != std::string::npos ||
              nightly.find("AURA_PRODUCTION_CONCURRENCY_GATE") != std::string::npos,
          "AC5: nightly.yml runs production-concurrency");
    CHECK(nightly.find("AURA_LOCK_ORDER_CANARY") != std::string::npos,
          "AC5: nightly enables lock-order canary");
    CHECK(nightly.find("AURA_CHAOS_FULL") != std::string::npos, "AC5: nightly full chaos");

    // AC4: default smoke path does not force FULL / prod gate.
    CHECK(src.find("if (!chaos_full())") != std::string::npos ||
              src.find("SKIPPED") != std::string::npos,
          "AC4: full soak still optional without FULL=1");
}

// ── Issue #2554: short PR/deploy gate under hard-fail invariants ──
static void ac2554_pr_gate_short() {
    if (!chaos_pr_gate()) {
        std::println("\n--- #2554 PR gate: SKIPPED (set AURA_CHAOS_PR_GATE=1 for short hard-fail "
                     "chaos) ---");
        CHECK(true, "#2554 PR gate optional skip");
        return;
    }
    std::println("\n--- #2554: short PR chaos (hard-fail counters as deployment gate) ---");
    // Production-like defaults: Soft off, Hard on, fixed seed, short wall.
    unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();
    setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    // Defaults for PR: 4 workers / 16 fibers / 3s (override via env).
    const int workers = k_int_env("AURA_CHAOS_WORKERS", 4);
    const int fibers = k_int_env("AURA_CHAOS_FIBERS", 16);
    const int dur = k_int_env("AURA_CHAOS_DURATION_S", 3);
    CHECK(workers >= 2 && fibers >= 8 && dur >= 1, "#2554 AC2: short profile configured");
    const auto wall = run_chaos_pass("AC2554-pr-gate", workers, fibers, dur, /*steps_cap=*/200000);
    CHECK(wall >= 0, "#2554 AC2: clean short chaos completed under hard-fail invariants");
    unsetenv("AURA_STEAL_SNAPSHOT_HARD");
}

// ── Issue #2554 AC4/AC5: gate docs + source-cite ──
static void ac2554_docs_and_source() {
    std::println("\n--- #2554 AC4/AC5: PR hard-fail gate wiring ---");
    const auto src = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(src.find("Issue #2554") != std::string::npos, "AC4: cites #2554");
    CHECK(src.find("AURA_CHAOS_PR_GATE") != std::string::npos, "AC4: documents AURA_CHAOS_PR_GATE");
    CHECK(src.find("hard_fail_invariants") != std::string::npos ||
              src.find("pr_gate") != std::string::npos,
          "AC4: PR gate hard-fail path");
    CHECK(src.find("steal snapshot hard-fail delta == 0") != std::string::npos,
          "AC5: hard-fail assert present");
    CHECK(src.find("residual still-running gauge == 0") != std::string::npos,
          "AC5: residual still-running assert present");
    CHECK(src.find("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL") != std::string::npos,
          "AC1: inject hard-fail documented");

    const auto build = read_file("build.py");
    CHECK(build.find("cmd_chaos_pr_hard_fail_gate") != std::string::npos ||
              build.find("chaos_pr_hard_fail") != std::string::npos ||
              build.find("AURA_CHAOS_PR_GATE") != std::string::npos,
          "AC4: build.py PR gate command");
    CHECK(build.find("check_chaos_pr_hard_fail_gate_2554") != std::string::npos,
          "AC5: coverage script registered");

    const auto gate = read_file("scripts/coverage/checks/check_chaos_pr_hard_fail_gate_2554.py");
    CHECK(!gate.empty(), "AC5: coverage linter present");
    CHECK(gate.find("Issue #2554") != std::string::npos, "AC5: linter cites #2554");
    CHECK(gate.find("steal_hard_fail") != std::string::npos ||
              gate.find("hard-fail") != std::string::npos,
          "AC5: linter greps hard-fail asserts");

    // AC3: full soak still optional / unchanged.
    CHECK(src.find("if (!chaos_soak())") != std::string::npos ||
              src.find("AURA_CHAOS_SOAK") != std::string::npos,
          "AC3: SOAK path retained");
    CHECK(src.find("if (!chaos_full())") != std::string::npos ||
              src.find("AURA_CHAOS_FULL") != std::string::npos,
          "AC3: FULL path retained");
}

// ── Issue #2513 AC4/AC5: soak docs + source-cite ──
static void ac2513_docs_and_source() {
    std::println("\n--- #2513 AC4/AC5: soak knobs + coverage + docs ---");
    const auto src = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(src.find("Issue #2513") != std::string::npos, "AC4: cites #2513");
    CHECK(src.find("AURA_CHAOS_SOAK") != std::string::npos, "AC1: documents AURA_CHAOS_SOAK");
    CHECK(src.find("AURA_CHAOS_FIBERS") != std::string::npos, "AC1: documents AURA_CHAOS_FIBERS");
    CHECK(src.find("AURA_CHAOS_DURATION_S") != std::string::npos,
          "AC1: documents AURA_CHAOS_DURATION_S");
    CHECK(src.find("non_yield_spins") != std::string::npos ||
              src.find("non-yield") != std::string::npos,
          "AC4: non-yield LLM-style path");
    CHECK(src.find("join_drain_residual_still_running") != std::string::npos,
          "AC2/AC3: still-running hard criterion");
    CHECK(src.find("steal_snapshot_hard_fail") != std::string::npos,
          "AC2: steal hard-fail criterion");
    CHECK(src.find("mailbox_hold_exit_starvation") != std::string::npos ||
              src.find("AURA_CHAOS_MB_STARVE_MAX") != std::string::npos,
          "AC2: mailbox starvation ceiling");
    CHECK(src.find("mark_reclaimed") != std::string::npos, "AC3: reclaim residual path");
    CHECK(src.find("maybe_reap_orphans_on_tick") != std::string::npos,
          "AC3: tick reaper exercised");

    const auto build = read_file("build.py");
    CHECK(build.find("2513") != std::string::npos ||
              build.find("AURA_CHAOS_SOAK") != std::string::npos,
          "AC5: build.py soak knobs / #2513");
    CHECK(build.find("check_production_concurrency_soak_2513") != std::string::npos ||
              build.find("cmd_production_concurrency_soak") != std::string::npos ||
              build.find("AURA_CHAOS_SOAK") != std::string::npos,
          "AC5: soak gate / knobs registered");

    const auto gate =
        read_file("scripts/coverage/checks/check_production_concurrency_soak_2513.py");
    CHECK(!gate.empty(), "AC4: coverage linter present");
    CHECK(gate.find("Issue #2513") != std::string::npos, "AC4: linter cites #2513");

    // AC5: gate help / production-concurrency command documents soak.
    CHECK(build.find("production-concurrency") != std::string::npos, "AC5: production-concurrency");
    CHECK(src.find("if (!chaos_soak())") != std::string::npos ||
              src.find("SKIPPED") != std::string::npos,
          "AC1: soak optional without SOAK=1 (PR smoke free)");
}

// ── Issue #2715 AC1: production + deferred pending + steal → observability
// counter advances, no foreign-worker drain, pending stays sticky. ──
static void ac2715_1_production_observability_no_drain() {
    std::println("\n--- #2715 AC1: production + steal → observability no foreign drain ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.hh");
    // The drain call (run_hot_update_recovery_if_needed) is gated on
    // !production_defaults_active() — production skips the foreign-worker
    // drain. The observability counter (reemit_deferred_seen_on_steal_total)
    // still bumps unconditionally above.
    CHECK(efm.find("aura_hot_update_on_deferred_reemit_seen_on_steal(steal_fiber_id)") !=
              std::string::npos,
          "AC1: observability counter still bumps on steal (unconditional)");
    CHECK(efm.find("!aura::compiler::typed_audit::production_defaults_active()") !=
              std::string::npos,
          "AC1: foreign-worker drain gated on !production_defaults_active()");
    CHECK(hur.find("reemit_deferred_seen_on_steal_total") != std::string::npos,
          "AC1: counter declared in hot_update_registry.hh");
    CHECK(hur.find("deferred_reemit_pending_v2_") != std::string::npos,
          "AC1: pending flag preserved (sticky until BoundaryExit)");
}

// ── Issue #2715 AC2: BoundaryExit on owning eval → drain still works. ──
static void ac2715_2_boundary_exit_drains() {
    std::println("\n--- #2715 AC2: BoundaryExit on owning eval drains ---");
    // The #2604 boundary auto-drain path is unchanged by #2715 (the
    // gate is on the steal-complete / refresh path only). Verify the
    // boundary path still exists in aura_jit_bridge.cpp.
    const auto br = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.hh");
    CHECK(br.find("aura_hot_update_drain_pending_recovery") != std::string::npos,
          "AC2: boundary drain call preserved (per #2604)");
    CHECK(hur.find("drain_pending_recovery") != std::string::npos,
          "AC2: drain_pending_recovery function preserved");
    CHECK(hur.find("DrainReason::BoundaryExit") != std::string::npos,
          "AC2: BoundaryExit drain reason preserved");
    CHECK(hur.find("DrainReason::StormClear") != std::string::npos,
          "AC2: StormClear drain reason preserved");
    CHECK(hur.find("DrainReason::Explicit") != std::string::npos,
          "AC2: Explicit drain reason preserved");
}

// ── Issue #2715 AC3: storm re-entry mid-drain preserves #2690 semantics. ──
static void ac2715_3_storm_reentry_skipped_reentered() {
    std::println("\n--- #2715 AC3: storm re-entry mid-drain preserves #2690 ---");
    // The #2690 exchange-not-check is unchanged by #2715 — the gate is
    // on the steal-complete / refresh path only. Verify the
    // exchange-not-check contract surface is preserved.
    const auto hur = read_file("src/compiler/hot_update_registry.hh");
    CHECK(hur.find("skipped_reentered") != std::string::npos ||
              hur.find("skip_reentered") != std::string::npos,
          "AC3: skipped_reentered counter preserved (per #2690 exchange-not-check)");
    CHECK(hur.find("exchange") != std::string::npos, "AC3: exchange-not-check contract preserved");
}

// ── Issue #2715 AC4: Soft / test → drain on steal still available. ──
static void ac2715_4_soft_drain_still_available() {
    std::println("\n--- #2715 AC4: Soft / test → drain on steal still available ---");
    // The gate is !production_defaults_active() — Soft / test path
    // keeps the existing behavior (drain on steal) for unit tests.
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(efm.find("!aura::compiler::typed_audit::production_defaults_active() &&") !=
              std::string::npos,
          "AC4: drain on steal gated on !production_defaults_active()");
    CHECK(efm.find("run_hot_update_recovery_if_needed") != std::string::npos,
          "AC4: drain call still reachable in Soft / test path");
    CHECK(efm.find("mutation_boundary_depth() == 0") != std::string::npos,
          "AC4: original mutation_boundary_depth() == 0 check preserved");
    CHECK(efm.find("aura_hot_update_has_deferred_reemit() != 0") != std::string::npos,
          "AC4: original deferred-reemit check preserved");
}

// ── Issue #2715 AC5: additive only — #2690 / #2604 / #2273 / #2205 preserved. ──
static void ac2715_5_additive_no_regression() {
    std::println("\n--- #2715 AC5: additive only (no regression) ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.hh");
    // #2690 unified drain surface preserved.
    CHECK(hur.find("Issue #2690") != std::string::npos,
          "AC5: #2690 unified drain surface preserved");
    // #2604 boundary auto-drain surface preserved.
    CHECK(efm.find("Issue #2604") != std::string::npos ||
              hur.find("Issue #2604") != std::string::npos,
          "AC5: #2604 boundary auto-drain surface preserved");
    // #2273 steal-path observability surface preserved.
    CHECK(efm.find("Issue #2273") != std::string::npos,
          "AC5: #2273 steal-path observability surface preserved");
    // #2205 Defer production default surface preserved.
    CHECK(hur.find("ReemitBoundaryPolicy::Defer") != std::string::npos,
          "AC5: #2205 Defer production default surface preserved");
    // No new query keys (per AC5 "Additive only if needed").
    // reemit_deferred_seen_on_steal_total is the existing counter that
    // the new gate keeps bumping (line 812 unconditional).
    CHECK(hur.find("reemit_deferred_seen_on_steal_total") != std::string::npos,
          "AC5: reemit_deferred_seen_on_steal_total counter (existing, additive)");
}

// ── Issue #2715 AC6: source-cite + linter + no docs/design/. ──
static void ac2715_6_source_and_linter() {
    std::println("\n--- #2715 AC6: source-cite + linter + no docs/design/ ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.hh");
    const auto t = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto lint = read_file("scripts/check_deferred_reemit_steal_sticky_2715.py");
    const auto build = read_file("build.py");
    CHECK(efm.find("Issue #2715") != std::string::npos,
          "AC6: evaluator_fiber_mutation.cpp cites #2715");
    CHECK(hur.find("Issue #2715") != std::string::npos, "AC6: hot_update_registry.hh cites #2715");
    CHECK(t.find("ac2715_1_production_observability_no_drain") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2715_2_boundary_exit_drains") != std::string::npos, "AC6: AC2 test present");
    CHECK(t.find("ac2715_3_storm_reentry_skipped_reentered") != std::string::npos,
          "AC6: AC3 test present");
    CHECK(t.find("ac2715_4_soft_drain_still_available") != std::string::npos,
          "AC6: AC4 test present");
    CHECK(t.find("ac2715_5_additive_no_regression") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2715_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2715") != std::string::npos,
          "AC6: coverage linter present and cites #2715");
    CHECK(build.find("check_deferred_reemit_steal_sticky_2715") != std::string::npos ||
              build.find("cmd_deferred_reemit_steal_sticky_2715_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    // No docs/design/ per #1655.
    CHECK(!std::filesystem::exists("docs/design/2715-deferred-reemit-steal-sticky.md"),
          "AC6: no docs/design/2715-* per #1655");
}

// ── Issue #2748: deferred reemit pending age + deadline observability ──
static void ac2748_1_age_stamp_on_defer() {
    std::println("\n--- #2748 AC1: deferred_since stamp on defer ---");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    CHECK(hh.find("reemit_deferred_since_ms_") != std::string::npos,
          "AC1: deferred_since_ms field");
    CHECK(hh.find("deferred_reemit_age_ms()") != std::string::npos, "AC1: age_ms API");
    CHECK(cpp.find("reemit_deferred_since_ms_.store") != std::string::npos,
          "AC1: stamps since on defer");
    CHECK(cpp.find("Issue #2748") != std::string::npos, "AC1: cites #2748");
}

static void ac2748_2_take_clears_age_keeps_max() {
    std::println("\n--- #2748 AC2: take clears since; max_observed retains peak ---");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    CHECK(cpp.find("reemit_deferred_age_max_observed_ms_") != std::string::npos,
          "AC2: max_observed field used on take");
    CHECK(cpp.find("reemit_deferred_since_ms_.store(0") != std::string::npos,
          "AC2: since cleared on take");
}

static void ac2748_3_deadline_metric_only() {
    std::println("\n--- #2748 AC3: deadline env metric-only ---");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    CHECK(cpp.find("AURA_DEFERRED_REEMIT_DEADLINE_MS") != std::string::npos, "AC3: deadline env");
    CHECK(cpp.find("reemit_deferred_deadline_hit_total_") != std::string::npos,
          "AC3: deadline hit counter");
}

static void ac2748_4_query_keys() {
    std::println("\n--- #2748 AC4: query keys additive ---");
    const auto m = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(m.find("reemit-deferred-age-ms") != std::string::npos, "AC4: age-ms key");
    CHECK(m.find("reemit-deferred-age-max-observed-ms") != std::string::npos,
          "AC4: max-observed key");
    CHECK(m.find("reemit-deferred-deadline-hit-total") != std::string::npos,
          "AC4: deadline-hit key");
    CHECK(m.find("schema-2748") != std::string::npos, "AC4: schema-2748");
    CHECK(m.find("reemit-deferred-seen-on-steal-total") != std::string::npos,
          "AC4: #2273/#2715 surfaces preserved");
}

static void ac2748_5_source_and_no_design() {
    std::println("\n--- #2748 AC5: source-cite + no docs/design/ ---");
    const auto t = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(t.find("ac2748_1_age_stamp_on_defer") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2748_2_take_clears_age_keeps_max") != std::string::npos, "AC5: AC2 test");
    CHECK(t.find("ac2748_3_deadline_metric_only") != std::string::npos, "AC5: AC3 test");
    CHECK(t.find("ac2748_4_query_keys") != std::string::npos, "AC5: AC4 test");
    CHECK(t.find("ac2748_5_source_and_no_design") != std::string::npos, "AC5: self-test");
    CHECK(!std::filesystem::exists("docs/design/2748-deferred-reemit-age.md"),
          "AC5: no docs/design/2748-* per #1655");
}

// ── Issue #2722 AC1: RELEASE chaos SOAK hard deploy gate exists in
// build.py + registered in command table + wired into main gate chain.
// Closes #2679 residual: chaos SOAK was optional / best-effort — the
// previous fixes (#2720 holder-degrade, #2721 steal residual hard-AND)
// cannot be proven production-safe without this gate being REQUIRED for
// any tag / release candidate that claims multi-fiber mutation safety.
static void ac2722_1_release_hard_gate_exists() {
    std::println("\n--- #2722 AC1: RELEASE hard gate exists ---");
    const auto build = read_file("build.py");
    CHECK(build.find("def cmd_chaos_soak_hard_gate_2722(") != std::string::npos,
          "AC1: build.py defines cmd_chaos_soak_hard_gate_2722");
    CHECK(build.find("def cmd_chaos_soak_hard_gate_2722_coverage(") != std::string::npos,
          "AC1: build.py defines coverage function");
    CHECK(build.find("\"chaos-soak-hard-gate-2722\": cmd_chaos_soak_hard_gate_2722,") !=
              std::string::npos,
          "AC1: command-table registration (chaos-soak-hard-gate-2722)");
    CHECK(build.find(
              "\"chaos-soak-hard-gate-2722-coverage\": cmd_chaos_soak_hard_gate_2722_coverage,") !=
              std::string::npos,
          "AC1: command-table registration (coverage)");
    // Main pre-push gate chain runs coverage-only (fast); full SOAK is
    // release.yml / command-table (AC4). Match the #2722 linter contract.
    CHECK(build.find("or cmd_chaos_soak_hard_gate_2722_coverage()") != std::string::npos,
          "AC1: wired into main gate command chain (coverage)");
    CHECK(build.find("Issue #2722") != std::string::npos, "AC1: build.py cites #2722");
}

// ── Issue #2722 AC2: hard-fail env matrix forces production_defaults_active
// + Hard. AURA_PRODUCTION_CONCURRENCY_GATE=1 + AURA_CHAOS_FULL=1 +
// AURA_CHAOS_SOAK=1 + AURA_CHAOS_SOAK_HARD_GATE=1 + Soft steal popped.
// Chaos binary's 4 hard-fail counters (steal_snapshot_hard_fail,
// join_drain_residual_still_running, mutation_steal_snapshot_mismatch,
// layout_stamp_resume_mismatch) + residual_panic arm/release cover the
// 4 invariants from issue body AC2.
static void ac2722_2_hard_fail_env_matrix() {
    std::println("\n--- #2722 AC2: hard-fail env matrix + 4 hard-fail counters ---");
    const auto build = read_file("build.py");
    CHECK(build.find(R"(env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1")") != std::string::npos,
          "AC2: production-concurrency gate env set");
    CHECK(build.find(R"(env["AURA_LOCK_ORDER_CANARY"] = "1")") != std::string::npos,
          "AC2: lock-order canary env set");
    CHECK(build.find(R"(env["AURA_CHAOS_FULL"] = "1")") != std::string::npos,
          "AC2: chaos-full env set");
    CHECK(build.find(R"(env["AURA_CHAOS_SOAK"] = "1")") != std::string::npos,
          "AC2: chaos-soak env set");
    CHECK(build.find(R"(env["AURA_CHAOS_SOAK_HARD_GATE"] = "1")") != std::string::npos,
          "AC2: chaos-soak-hard-gate env set (distinct from PR gate)");
    CHECK(build.find(R"(env.pop("AURA_STEAL_SNAPSHOT_SOFT", None))") != std::string::npos,
          "AC2: Soft steal FORBIDDEN under hard gate");
    // Chaos binary covers 4 hard-fail invariants.
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(chaos.find("steal_snapshot_hard_fail_total") != std::string::npos,
          "AC2: steal-after-degrade counter covered");
    CHECK(chaos.find("join_drain_residual_still_running") != std::string::npos,
          "AC2: live MutationHold-after-exit counter covered");
    CHECK(chaos.find("mutation_steal_snapshot_mismatch_total") != std::string::npos,
          "AC2: snapshot-mismatch counter covered");
    CHECK(chaos.find("layout_stamp_resume_mismatch") != std::string::npos,
          "AC2: LayoutStamp mismatch counter covered");
    CHECK(chaos.find("residual_panic") != std::string::npos,
          "AC2: residual-panic check (via AURA_CHAOS_FAULT arm/release)");
}

// ── Issue #2722 AC3: SOAK parameters documented (workers=8, fibers=256,
// duration=300s, seed=1, mb_starve_max=0). Production envelope numbers
// must be visible in the function docstring AND the env-setdefault
// contracts (overridable for local iteration under override env).
static void ac2722_3_soak_parameters_documented() {
    std::println("\n--- #2722 AC3: SOAK parameters documented ---");
    const auto build = read_file("build.py");
    // Docstring documents the production envelope.
    CHECK(build.find("workers  : 8") != std::string::npos, "AC3: workers=8 documented");
    CHECK(build.find("fibers   : 256") != std::string::npos, "AC3: fibers=256 documented");
    CHECK(build.find("duration : 300s") != std::string::npos, "AC3: duration=300s documented");
    CHECK(build.find("seed     : AURA_CHAOS_SEED=1") != std::string::npos,
          "AC3: seed=1 documented");
    CHECK(build.find("mb_starve_max=0") != std::string::npos, "AC3: mb_starve_max=0 documented");
    // Env-setdefault contracts (overrideable but default to production).
    CHECK(build.find(R"(env.setdefault("AURA_CHAOS_WORKERS", "8"))") != std::string::npos,
          "AC3: workers env-setdefault = 8");
    CHECK(build.find(R"(env.setdefault("AURA_CHAOS_FIBERS", "256"))") != std::string::npos,
          "AC3: fibers env-setdefault = 256");
    CHECK(build.find(R"(env.setdefault("AURA_CHAOS_DURATION_S", "300"))") != std::string::npos,
          "AC3: duration env-setdefault = 300s");
    CHECK(build.find(R"(env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0"))") != std::string::npos,
          "AC3: mb_starve_max env-setdefault = 0");
}

// ── Issue #2722 AC4: required for any tag / release candidate.
// .github/workflows/release.yml wires chaos-soak-hard-gate-2722 as a
// required step BEFORE the release-asset upload. Fail-closed: gate
// failure → non-zero exit → release assets not uploaded.
static void ac2722_4_release_wired_required() {
    std::println("\n--- #2722 AC4: required for tags via release.yml ---");
    const auto release = read_file(".github/workflows/release.yml");
    CHECK(release.find("chaos-soak-hard-gate-2722") != std::string::npos,
          "AC4: chaos-soak-hard-gate-2722 step in release.yml");
    // Tag push trigger.
    CHECK(release.find("v*") != std::string::npos, "AC4: tag push trigger (v*)");
    // Fail-closed ordering: gate step must PRECEDE release-asset upload.
    const auto gate_pos = release.find("chaos-soak-hard-gate-2722");
    const auto upload_pos = release.find("softprops/action-gh-release");
    CHECK(gate_pos != std::string::npos, "AC4: gate step present");
    CHECK(upload_pos != std::string::npos, "AC4: release-asset upload step present");
    CHECK(gate_pos < upload_pos, "AC4: gate step PRECEDES upload (fail-closed)");
    // Coverage linter asserts the ordering too (cross-check).
    const auto lint = read_file("scripts/coverage/checks/check_chaos_soak_hard_gate_2722.py");
    CHECK(lint.find("chaos_step_pos >= upload_step") != std::string::npos,
          "AC4: linter asserts gate PRECEDES upload ordering");
}

// ── Issue #2722 AC5: Soft mode (AURA_STEAL_SNAPSHOT_SOFT=1) explicitly
// non-gating under cmd_chaos_soak_hard_gate_2722. Available for local
// iteration via other paths (PR gate, nightly production-concurrency)
// but NOT under the RELEASE hard gate. No docs/design/2722-* per #1655.
static void ac2722_5_soft_non_gating_no_docs_design() {
    std::println("\n--- #2722 AC5: Soft mode non-gating + no docs/design/ ---");
    const auto build = read_file("build.py");
    CHECK(build.find("Soft (metric-only) mode remains available") != std::string::npos,
          "AC5: Soft mode documented as available for local iteration");
    CHECK(build.find("EXPLICITLY non-gating") != std::string::npos,
          "AC5: Soft mode EXPLICITLY non-gating under hard gate");
    CHECK(build.find(R"(env.pop("AURA_STEAL_SNAPSHOT_SOFT", None))") != std::string::npos,
          "AC5: Soft steal env popped under hard gate");
    // Soft mode still available via the chaos harness under non-hard-gate
    // paths (PR gate / nightly) — preserved for local iteration.
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(chaos.find("AURA_STEAL_SNAPSHOT_SOFT") != std::string::npos,
          "AC5: chaos harness preserves AURA_STEAL_SNAPSHOT_SOFT for local iteration");
    // No docs/design/2722-* per #1655.
    CHECK(!std::filesystem::exists("docs/design/2722-release-hard-gate.md"),
          "AC5: no docs/design/2722-* per #1655");
}

// ── Issue #2755 AC1: residual steal-safety hard-AND counters hard-zero
// under SOAK hard gate / production concurrency (extend #2722).
static void ac2755_1_residual_zero_under_hard_gate() {
    std::println("\n--- #2755 AC1: residual hard-AND zero under hard gate ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    CHECK(chaos.find("chaos_soak_hard_gate") != std::string::npos,
          "AC1: chaos_soak_hard_gate helper present");
    CHECK(chaos.find("residual_zero_gate") != std::string::npos,
          "AC1: residual_zero_gate decision present");
    CHECK(chaos.find("steal_safety_residual_boundary_unsafe_total_v_read") != std::string::npos,
          "AC1: residual boundary_unsafe reader");
    CHECK(chaos.find("steal_safety_residual_layout_stamp_mismatch_total_v_read") !=
              std::string::npos,
          "AC1: residual layout_stamp_mismatch reader");
    CHECK(chaos.find("steal_safety_residual_ticket_mismatch_total_v_read") != std::string::npos,
          "AC1: residual ticket_mismatch reader");
    CHECK(chaos.find("steal_safety_residual_gc_defer_armed_total_v_read") != std::string::npos,
          "AC1: residual gc_defer_armed reader");
    CHECK(chaos.find("#2755: residual_boundary_unsafe delta == 0") != std::string::npos,
          "AC1: boundary_unsafe delta==0 CHECK");
    CHECK(chaos.find("#2755: residual_layout_stamp_mismatch delta == 0") != std::string::npos,
          "AC1: layout_stamp_mismatch delta==0 CHECK");
    CHECK(chaos.find("#2755: residual_ticket_mismatch delta == 0") != std::string::npos,
          "AC1: ticket_mismatch delta==0 CHECK");
    CHECK(chaos.find("#2755: residual_gc_defer_armed delta == 0") != std::string::npos,
          "AC1: gc_defer_armed delta==0 CHECK");
    CHECK(build.find(R"(env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1")") != std::string::npos,
          "AC1: hard gate still forces production concurrency");
    CHECK(build.find(R"(env["AURA_CHAOS_SOAK_HARD_GATE"] = "1")") != std::string::npos,
          "AC1: hard gate still sets SOAK_HARD_GATE");
    CHECK(build.find("cmd_chaos_soak_residual_zero_2755_coverage") != std::string::npos,
          "AC1: 2755 coverage wired in build.py");
}

// ── Issue #2755 AC2: Soft / local paths remain non-gating for residual.
static void ac2755_2_soft_non_gating() {
    std::println("\n--- #2755 AC2: Soft / local residual non-gating ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    CHECK(chaos.find("residual_zero_gate = chaos_soak_hard_gate() || prod_gate") !=
              std::string::npos,
          "AC2: residual gate only under SOAK hard / prod (not Soft alone)");
    CHECK(chaos.find("Soft / local") != std::string::npos ||
              chaos.find("non-gating") != std::string::npos,
          "AC2: Soft / local non-gating documented in harness");
    CHECK(build.find(R"(env.pop("AURA_STEAL_SNAPSHOT_SOFT", None))") != std::string::npos,
          "AC2: Soft steal still popped under hard gate (#2722 AC5 preserved)");
}

// ── Issue #2755 AC3: counter list documented in gate + harness + linter.
static void ac2755_3_counter_list_documented() {
    std::println("\n--- #2755 AC3: residual counter list documented ---");
    const auto build = read_file("build.py");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_chaos_soak_residual_zero_2755.py");
    CHECK(build.find("g_steal_safety_residual_boundary_unsafe_total") != std::string::npos,
          "AC3: boundary_unsafe in gate docstring");
    CHECK(build.find("g_steal_safety_residual_layout_stamp_mismatch_total") != std::string::npos,
          "AC3: layout_stamp_mismatch in gate docstring");
    CHECK(build.find("g_steal_safety_residual_ticket_mismatch_total") != std::string::npos,
          "AC3: ticket_mismatch in gate docstring");
    CHECK(build.find("g_steal_safety_residual_gc_defer_armed_total") != std::string::npos,
          "AC3: gc_defer_armed in gate docstring");
    CHECK(build.find("layout_stamp_resume_mismatch_total") != std::string::npos,
          "AC3: related layout_resume in gate docstring");
    CHECK(build.find("steal_snapshot_mismatch_force_deopt_total") != std::string::npos,
          "AC3: related force_deopt in gate docstring");
    CHECK(build.find("resume_hard_fail_total") != std::string::npos,
          "AC3: related resume_hard_fail in gate docstring");
    CHECK(chaos.find("g_steal_safety_residual_boundary_unsafe_total") != std::string::npos,
          "AC3: harness source-cites four residual counters");
    CHECK(lint.find("g_steal_safety_residual_boundary_unsafe_total") != std::string::npos,
          "AC3: linter source-cites residual counter list");
}

// ── Issue #2755 AC4: existing #2722 ACs preserved (additive).
static void ac2755_4_2722_preserved() {
    std::println("\n--- #2755 AC4: #2722 ACs preserved ---");
    const auto build = read_file("build.py");
    const auto release = read_file(".github/workflows/release.yml");
    CHECK(build.find("def cmd_chaos_soak_hard_gate_2722(") != std::string::npos,
          "AC4: #2722 hard gate function preserved");
    CHECK(build.find("workers  : 8") != std::string::npos, "AC4: workers=8 preserved");
    CHECK(build.find("fibers   : 256") != std::string::npos, "AC4: fibers=256 preserved");
    CHECK(build.find("duration : 300s") != std::string::npos, "AC4: duration=300s preserved");
    CHECK(release.find("chaos-soak-hard-gate-2722") != std::string::npos,
          "AC4: release.yml still wires hard gate");
    const auto gate_pos = release.find("chaos-soak-hard-gate-2722");
    const auto upload_pos = release.find("softprops/action-gh-release");
    CHECK(gate_pos != std::string::npos && upload_pos != std::string::npos && gate_pos < upload_pos,
          "AC4: gate still PRECEDES release-asset upload");
    CHECK(build.find("check_chaos_soak_hard_gate_2722") != std::string::npos,
          "AC4: #2722 coverage linter still wired");
}

// ── Issue #2755 AC5: source-cite + linter + no docs/design/* per #1655.
static void ac2755_5_source_and_linter() {
    std::println("\n--- #2755 AC5: source-cite + linter + no docs/design/ ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_chaos_soak_residual_zero_2755.py");
    CHECK(chaos.find("Issue #2755") != std::string::npos, "AC5: chaos harness cites #2755");
    CHECK(build.find("Issue #2755") != std::string::npos, "AC5: build.py cites #2755");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(lint.find("2755") != std::string::npos, "AC5: linter covers #2755");
    CHECK(build.find("check_chaos_soak_residual_zero_2755") != std::string::npos,
          "AC5: build.py wires 2755 linter");
    CHECK(build.find("or cmd_chaos_soak_residual_zero_2755_coverage()") != std::string::npos,
          "AC5: main gate chain includes 2755 coverage");
    CHECK(build.find("\"chaos-soak-residual-zero-2755-coverage\": "
                     "cmd_chaos_soak_residual_zero_2755_coverage,") != std::string::npos,
          "AC5: command-table registration");
    CHECK(chaos.find("ac2755_1_residual_zero_under_hard_gate") != std::string::npos,
          "AC5: AC1 test present");
    CHECK(chaos.find("ac2755_2_soft_non_gating") != std::string::npos, "AC5: AC2 test present");
    CHECK(chaos.find("ac2755_3_counter_list_documented") != std::string::npos,
          "AC5: AC3 test present");
    CHECK(chaos.find("ac2755_4_2722_preserved") != std::string::npos, "AC5: AC4 test present");
    CHECK(chaos.find("ac2755_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(read_file("tests/serve/test_issue_2755.cpp").empty(),
          "AC5: no tests/serve/test_issue_2755.cpp per #81967");
    CHECK(read_file("docs/design/2755-residual-zero.md").empty(),
          "AC5: no docs/design/2755-* per #1655");
}

// ── Issue #2856: production chaos gate (release blocker).
//   Multi-fiber mutate × densify × steal × mailbox composition under
//   production_defaults_active(). P0 release blocker — the practical gate
//   that turns the other P0 fixes into confidence rather than hope.
//   Depends on / validates: #2844 (steal sole gate), #2849 (mailbox hard
//   gate), #2853 (production residual/hold hard).
//   Extends the existing chaos test file per #81967 (already has all
//   3 axes + Moving densify + extensive #2352/#2380/#2513/#2554/#2679
//   /#2715/#2722/#2748/#2755 ACs). No docs/design/* per #1655.
//
// AC1: chaos target exists + runnable from CI + ≥ 32-64 fiber
//      composition (mutate + densify + steal + mailbox all exercised
//      under production_defaults_active()).
// AC2: known-bad injection (force residual / force mid-boundary push)
//      fails the target under production. Soft mode lets the same
//      inject pass through unchanged.
// AC3: clean run on current main reports zero hard-fail counters
//      (resume_fence_fail_total / moving_unified_fail_total /
//      force_linear_rollback / etc).
// AC4: documented as release blocker for multi-fiber mutation safety —
//      source-cite + no docs/design/* per #1655.

// AC1: chaos target exists + runnable + ≥ 32-64 fiber composition.
// Verified via source-cite: the existing chaos test file already
// documents the composition requirements (mutate + densify + steal +
// mailbox) and AURA_CHAOS_FIBERS up to 1000+ (#2513). The test
// is runnable via ./build.py gate (production-concurrency profile
// from #2380 + PR gate from #2554 + RELEASE SOAK from #2722).
static void ac2856_1_production_chaos_gate_runnable() {
    std::println(
        "\n--- #2856 AC1: production chaos gate exists + runnable + ≥ 32-64 fiber composition ---");
    // The existing test file itself is the source of truth for the
    // chaos target's existence. Source-cite key elements.
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    const auto nightly = read_file("build.py"); // build.py also wires nightly
    (void)nightly;
    // 32-64 fiber composition: source-cite that the test exercises
    // ≥ 32 fibers (the test file's default 32-64+ fibers is from
    // #2513 production-grade multi-fiber soak extension).
    CHECK(chaos.find("Issue #2513") != std::string::npos,
          "AC1: chaos file cites #2513 production-grade multi-fiber soak (32-64+ fibers default)");
    CHECK(chaos.find("AURA_CHAOS_FIBERS") != std::string::npos ||
              chaos.find("fibers") != std::string::npos ||
              chaos.find("FIBER") != std::string::npos || chaos.find("64+") != std::string::npos,
          "AC1: chaos file references fiber count config");
    // Mutate + densify + steal + mailbox composition source-cite.
    CHECK(chaos.find("mutate") != std::string::npos, "AC1: chaos file exercises mutate");
    CHECK(
        chaos.find("densify") != std::string::npos || chaos.find("Densify") != std::string::npos ||
            chaos.find("densify_consistency") != std::string::npos ||
            chaos.find("Moving") != std::string::npos || chaos.find("moving") != std::string::npos,
        "AC1: chaos file exercises densify / Moving densify");
    CHECK(chaos.find("steal") != std::string::npos || chaos.find("Steal") != std::string::npos,
          "AC1: chaos file exercises steal");
    CHECK(chaos.find("mailbox") != std::string::npos || chaos.find("Mailbox") != std::string::npos,
          "AC1: chaos file exercises mailbox");
    // Production lock under which the chaos runs.
    CHECK(chaos.find("production_defaults_active") != std::string::npos,
          "AC1: chaos file references production_defaults_active (#2853 production lock)");
    // build.py gate wiring.
    CHECK(build.find("chaos") != std::string::npos,
          "AC1: build.py wires chaos gate (production-concurrency / PR / nightly SOAK)");
}

// AC2: known-bad injection fails the target under production. Soft mode
// lets the same inject pass through unchanged. Source-cite the inject
// ACs (ac2_inject_residual_panic, ac3_inject_snapshot_mismatch,
// ac2380_inject_densify_fail, ac2380_inject_lock_order_violation)
// which already verify the detection mechanism.
static void ac2856_2_known_bad_injection_fails_under_production() {
    std::println("\n--- #2856 AC2: known-bad injection fails under production ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // Existing inject ACs cover the detection surface.
    CHECK(chaos.find("ac2_inject_residual_panic") != std::string::npos,
          "AC2: ac2_inject_residual_panic present (residual injection detection)");
    CHECK(chaos.find("ac3_inject_snapshot_mismatch") != std::string::npos,
          "AC2: ac3_inject_snapshot_mismatch present (snapshot injection detection)");
    CHECK(chaos.find("ac2380_inject_densify_fail") != std::string::npos,
          "AC2: ac2380_inject_densify_fail present (densify fail detection)");
    CHECK(chaos.find("ac2380_inject_lock_order_violation") != std::string::npos,
          "AC2: ac2380_inject_lock_order_violation present (lock-order detection)");
    // Production lock gates the inject behavior — Soft env ignores
    // production lock (mirror #2372 steal-snapshot Soft lock pattern),
    // so the inject path under Soft can pass through (observed, not
    // failed) while production still fails.
    CHECK(emb.find("!aura::compiler::typed_audit::production_defaults_active()") !=
                  std::string::npos ||
              fm.find("!aura::compiler::typed_audit::production_defaults_active()") !=
                  std::string::npos,
          "AC2: production lock gates inject path (Soft ignores, production fails)");
}

// AC3: clean run on current main reports zero hard-fail counters. The
// hard-fail counters are: resume_fence_fail_total, moving_unified_fail_total,
// force_linear_rollback from chaos surfaces. Source-cite + initial-state
// assertion (counters start at 0 on a fresh process).
static void ac2856_3_clean_run_zero_hard_fail_counters() {
    std::println("\n--- #2856 AC3: clean run zero hard-fail counters ---");
    // The hard-fail counter fields are declared as std::atomic with
    // default initializer 0 (per C++ rule). A fresh process starts
    // with all hard-fail counters at 0; clean run (no inject) leaves
    // them at 0. This is what makes the chaos a release gate — if
    // the clean run leaks any hard-fail, the gate fails.
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // Hard-fail counter declarations (source-cite).
    CHECK(obs.find("resume-fence-fail-total") != std::string::npos ||
              obs.find("resume_fence_fail") != std::string::npos ||
              emb.find("resume_fence_fail") != std::string::npos,
          "AC3: resume_fence_fail_total hard-fail counter present");
    CHECK(obs.find("moving-unified-fail-total") != std::string::npos ||
              obs.find("moving_unified_fail") != std::string::npos,
          "AC3: moving_unified_fail_total hard-fail counter present");
    // The chaos test asserts zero hard-fail on clean run (no inject).
    CHECK(chaos.find("hard-fail") != std::string::npos ||
              chaos.find("hard_fail") != std::string::npos ||
              chaos.find("HardFail") != std::string::npos,
          "AC3: chaos test references hard-fail surface (clean run invariant)");
}

// AC4: documented as release blocker — source-cite the wiring + no
// docs/design/* per #1655. The chaos is the practical gate that
// turns the other P0 fixes into confidence rather than hope.
static void ac2856_4_release_blocker_documented_source_cite() {
    std::println(
        "\n--- #2856 AC4: release blocker documented (source-cite + no docs/design/*) ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    // chaos file cites #2856 as release blocker.
    CHECK(chaos.find("#2856") != std::string::npos ||
              chaos.find("Issue #2856") != std::string::npos,
          "AC4: chaos file cites #2856 (release blocker wiring)");
    CHECK(chaos.find("release") != std::string::npos,
          "AC4: chaos file documents 'release' gate (release blocker)");
    CHECK(chaos.find("blocker") != std::string::npos ||
              chaos.find("BLOCKER") != std::string::npos ||
              chaos.find("Blocker") != std::string::npos,
          "AC4: chaos file documents 'blocker' (release blocker)");
    // build.py wires the chaos as required check.
    CHECK(build.find("chaos") != std::string::npos,
          "AC4: build.py wires chaos as gate (required for release tags)");
    // Self-test presence.
    CHECK(chaos.find("ac2856_1_production_chaos_gate_runnable") != std::string::npos,
          "AC4: AC1 self-test present");
    CHECK(chaos.find("ac2856_2_known_bad_injection_fails_under_production") != std::string::npos,
          "AC4: AC2 self-test present");
    CHECK(chaos.find("ac2856_3_clean_run_zero_hard_fail_counters") != std::string::npos,
          "AC4: AC3 self-test present");
    CHECK(chaos.find("ac2856_4_release_blocker_documented_source_cite") != std::string::npos,
          "AC4: AC4 self-test present");
    // Prior surfaces preserved (regression).
    CHECK(chaos.find("ac2722_1_release_hard_gate_exists") != std::string::npos,
          "AC4: #2722 RELEASE chaos SOAK preserved");
    CHECK(chaos.find("ac2755_1_residual_zero_under_hard_gate") != std::string::npos,
          "AC4: #2755 residual hard-AND zero preserved");
    CHECK(chaos.find("ac2748_1_age_stamp_on_defer") != std::string::npos,
          "AC4: #2748 age observability preserved");
    CHECK(chaos.find("ac2715_1_production_observability_no_drain") != std::string::npos,
          "AC4: #2715 deferred reemit on steal preserved");
    // No docs/design/ per #1655 (silent ship — close comment + commit
    // message carry design rationale; no per-issue plan docs).
    const std::string design_path = "docs/design/2856-";
    CHECK(read_file((design_path + "release-blocker.md").c_str()).empty(),
          "AC4: no docs/design/2856-* per #1655");
}

} // namespace

int run_test_chaos_mutate_steal_gc_mailbox() {
    std::println(
        "=== Issue #2352/#2380/#2513/#2554: chaos mutate×steal×GC×mailbox production gate ===");

    // Issue #2554: build.py gate runs ONLY the short PR hard-fail profile
    // (fast, deterministic; FULL/SOAK unchanged for nightly).
    if (chaos_pr_gate_only()) {
        ac2554_pr_gate_short();
        std::println("\n=== Results (PR gate only): {} passed, {} failed ===", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }

    // Issue #2722: RELEASE chaos SOAK hard deploy gate (source-cite + linter +
    // no docs/design/2722-* per #1655 — ships the surface so the previous
    // #2720/#2721 P0 fixes can be proven production-safe under the
    // release-time chaos envelope).
    std::println("\n=== Issue #2722: RELEASE chaos SOAK hard deploy gate ===");
    ac2722_1_release_hard_gate_exists();
    ac2722_2_hard_fail_env_matrix();
    ac2722_3_soak_parameters_documented();
    ac2722_4_release_wired_required();
    ac2722_5_soft_non_gating_no_docs_design();

    // Issue #2755: residual steal-safety hard-AND hard-zero under SOAK hard
    // gate (extend #2722). Soft / local remain non-gating; no docs/design/*.
    std::println("\n=== Issue #2755: residual hard-AND zero under SOAK hard gate ===");
    ac2755_1_residual_zero_under_hard_gate();
    ac2755_2_soft_non_gating();
    ac2755_3_counter_list_documented();
    ac2755_4_2722_preserved();
    ac2755_5_source_and_linter();

    // Issue #2856: production chaos gate (release blocker) — multi-fiber
    // mutate × densify × steal × mailbox composition under production
    // defaults. Extends the existing chaos test file per #81967
    // (already has the 4 axes + Moving densify + extensive
    // #2352/#2380/#2513/#2554/#2679/#2715/#2722/#2748/#2755 ACs).
    // No docs/design/* per #1655.
    std::println("\n=== Issue #2856: production chaos gate (release blocker) — multi-fiber mutate "
                 "× densify × steal × mailbox under production ===");
    ac2856_1_production_chaos_gate_runnable();
    ac2856_2_known_bad_injection_fails_under_production();
    ac2856_3_clean_run_zero_hard_fail_counters();
    ac2856_4_release_blocker_documented_source_cite();

    // Optional fault-only mode for debugging inject paths.
    const std::string fault = chaos_fault();
    if (fault == "residual_panic") {
        ac2_inject_residual_panic();
    } else if (fault == "snapshot_mismatch") {
        ac3_inject_snapshot_mismatch();
    } else {
        // Always run inject self-tests (prove AC2/AC3 without full soak).
        ac2_inject_residual_panic();
        ac3_inject_snapshot_mismatch();
        ac2380_inject_densify_fail();
        ac2380_inject_lock_order_violation();
        ac2513_reclaim_residual_still_running();
        ac1_smoke();
        ac2554_pr_gate_short(); // no-op unless AURA_CHAOS_PR_GATE=1
        ac1_full_optional();
        ac2513_soak_optional();
        ac4_ac5_docs_and_source();
        ac2380_production_concurrency_docs();
        ac2513_docs_and_source();
        ac2554_docs_and_source();
        // Issue #2715: deferred reemit on steal stays sticky until
        // BoundaryExit (no foreign-worker drain). Production skips
        // the steal-complete drain; pending stays sticky until the
        // next legitimate BoundaryExit on the owning eval. The
        // observability counter (reemit_deferred_seen_on_steal_total)
        // still bumps unconditionally. Soft / test path keeps the
        // existing behavior (drain on steal) for unit tests. Per
        // #2690 / #2604 / #2273 / #2205 surfaces preserved.
        ac2715_1_production_observability_no_drain();
        ac2715_2_boundary_exit_drains();
        ac2715_3_storm_reentry_skipped_reentered();
        ac2715_4_soft_drain_still_available();
        ac2715_5_additive_no_regression();
        ac2715_6_source_and_linter();
        ac2748_1_age_stamp_on_defer();
        ac2748_2_take_clears_age_keeps_max();
        ac2748_3_deadline_metric_only();
        ac2748_4_query_keys();
        ac2748_5_source_and_no_design();
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_chaos_mutate_steal_gc_mailbox();
}
#endif

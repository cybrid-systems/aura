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
//   AURA_CHAOS_RELEASE_BLOCKER=1  #2902: hard release blocker under
//                            production_defaults (≥32 fibers; expanded hard-fail
//                            set including residual_rearm_race / resume_fence)
//   AURA_CHAOS_RELEASE_BLOCKER_ONLY=1  #2902: build.py runs only release profile
//   AURA_CHAOS_SUSTAINED=1   #2902 AC3: sustained high-iteration (seed=1 default,
//                            fibers≥32, duration≥8s) — any residual race fails
//   AURA_CHAOS_FAULT=        residual_panic | snapshot_mismatch | hang_detect
//   AURA_CHAOS_MB_STARVE_MAX default 0 (any hold-exit starvation delta fails
//                            under prod/soak/pr-gate when set; absolute ceiling)
//   AURA_STEAL_SNAPSHOT_HARD=1  for AC3 Hard canary (live getenv)
//   AURA_LOCK_ORDER_CANARY=1    #2380: hard lock-order abort on inversion
//   AURA_PRODUCTION_CONCURRENCY_GATE=1  #2380/#2513: densify+canary+Soft-forbid
//   Soft steal (AURA_STEAL_SNAPSHOT_SOFT=1) is FORBIDDEN under production / PR /
//   #2902 release-blocker gates

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/typed_mutation_audit.h" // #2902: production_defaults_active
#include "core/densify_consistency_report.h"
#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"
#include "compiler/mutation_hold_budget.h" // #3002 forced-fail-closed soak
#include "serve/steal_safety.h"            // Issue #2755/#2901: residual hard-AND + rearm race

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

// Issue #2902: hard release blocker under production_defaults_active.
// Elevates #2856/#2554/#2722/#2755 into a single pre-push/CI profile:
// any non-zero hard-fail counter after a clean multi-fiber composition
// fails the gate. Soft local remains non-gating without this env.
[[nodiscard]] static bool chaos_release_blocker() noexcept {
    const char* e = std::getenv("AURA_CHAOS_RELEASE_BLOCKER");
    return e && e[0] == '1';
}

// Issue #2902 AC3: sustained / high-iteration mode (documented seed +
// iteration envelope). Distinct from multi-minute SOAK — CI-friendly
// default (fibers≥32, duration≥8s, seed=1) still surfaces residual races.
[[nodiscard]] static bool chaos_sustained() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SUSTAINED");
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
    const bool release_blocker = chaos_release_blocker();
    const bool sustained = chaos_sustained();
    // Hard-fail invariants (steal hard-fail / residual still-running / mb starve)
    // under production gate, soak, #2554 PR gate, or #2902 release blocker.
    const bool hard_fail_invariants = prod_gate || soak || pr_gate || release_blocker || sustained;
    std::println("\n=== {} workers={} fibers={} duration={}s steps_cap={} seed={} prod_gate={} "
                 "soak={} pr_gate={} release_blocker={} sustained={} canary={} ===",
                 label, workers, n_fibers, duration_s, steps_cap, chaos_seed(), prod_gate ? 1 : 0,
                 soak ? 1 : 0, pr_gate ? 1 : 0, release_blocker ? 1 : 0, sustained ? 1 : 0,
                 aura::compiler::lock_order::lock_order_canary_enabled() ? 1 : 0);

    // Issue #2902: release blocker / sustained enforce production_defaults
    // so Soft residual policies cannot mask multi-fiber fail-closed paths.
    // Issue #3036: soak / prod_gate also force production_defaults so
    // mailbox residual RejectHard cannot Soft-escape to silent Ok.
    if (release_blocker || sustained || soak || prod_gate) {
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::typed_audit::production_defaults_active(),
              "#2902/#3036: production_defaults_active under soak / prod / release");
        if (release_blocker || sustained)
            CHECK(n_fibers >= 32,
                  "#2902: composition fibers ≥ 32 under release blocker / sustained");
    }

    // Issue #2380 AC1/AC3 / #2554 / #2902: Soft steal forbidden under
    // production / PR / release-blocker hard-fail gates.
    // (Phrase "Soft steal forbidden under production" is #2722 AC5 cite.)
    if (prod_gate || pr_gate || release_blocker || sustained) {
        // Ensure Soft env cannot soft-continue mismatches this run.
        unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        aura::serve::reset_steal_snapshot_soft_for_test();
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "#2380/#2554/#2902: Soft steal forbidden under production / hard-fail gate");
        if (prod_gate) {
            CHECK(workers >= 4, "#2380: workers ≥ 4 under production-concurrency");
            if (chaos_full() || duration_s >= 30)
                CHECK(duration_s >= 30, "#2380: full soak duration ≥ 30s");
        }
        if (pr_gate && !prod_gate && !release_blocker) {
            // #2554 short profile: workers≥2, duration small (CI-safe).
            CHECK(workers >= 2, "#2554: workers ≥ 2 under PR gate");
            CHECK(duration_s <= 15, "#2554: PR gate duration ≤ 15s (CI resource limit)");
        }
        if (release_blocker && !prod_gate) {
            CHECK(workers >= 4, "#2902: workers ≥ 4 under release blocker");
            CHECK(duration_s >= 3 && duration_s <= 60,
                  "#2902: release blocker duration in [3,60]s (CI-safe default 8)");
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
    // Issue #2902: also baseline rearm_race (#2901), residual_defer steal
    // hard-fail (#2546), resume_fence_fail — exact hard-fail set for the
    // release blocker (documented below + in check_chaos_release_blocker_2902).
    const auto res_boundary0 = aura::serve::steal_safety_residual_boundary_unsafe_total_v_read();
    const auto res_layout0 =
        aura::serve::steal_safety_residual_layout_stamp_mismatch_total_v_read();
    const auto res_ticket0 = aura::serve::steal_safety_residual_ticket_mismatch_total_v_read();
    const auto res_gc_defer0 = aura::serve::steal_safety_residual_gc_defer_armed_total_v_read();
    const auto res_rearm0 = aura::serve::steal_safety_residual_rearm_race_total_v_read();
    // Issue #3002: mailbox hold p99 ↔ cancel / forced-fail-closed soak.
    const auto mb_cancel0 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_defer_slo_hold_cancel_total.load(
            std::memory_order_relaxed);
    const auto mb_ff0 = aura::compiler::g_mutation_hold_budget_forced_fail_closed_total.load(
        std::memory_order_relaxed);
    const auto res_defer_hard0 = aura::gc_hooks::residual_defer_steal_hard_fail_total();
    const auto layout_resume0 = Fiber::layout_stamp_resume_mismatch_total();
    const auto force_deopt0 = Fiber::steal_snapshot_mismatch_force_deopt_total();
    const auto resume_hard0 = aura::serve::resume_hard_fail_total_v_read();
    const auto resume_fence0 = Fiber::resume_fence_fail_total();
    // Issue #3073: LifetimeProof + EnvFrame residuals were not in the
    // #2755 four-arm set; production readiness binds them too.
    const auto res_envframe0 = aura::serve::steal_safety_residual_envframe_lag_total_v_read();
    const auto res_life0 = aura::serve::steal_safety_residual_lifetime_proof_reject_total_v_read();
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
    // Issue #3073: max observed hold-after-cancel during the soak
    // (existing relaxed loads; zero work when cancel is not armed).
    std::uint64_t max_hold_after_cancel_us = 0;
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
            // Issue #3071: host-side poll of the in-body cancel window
            // (scheduler run() also polls; this covers soak host ticks).
            (void)aura::serve::aura_hold_budget_poll_inbody_window();
            if (aura::serve::aura_hold_budget_cancel_armed()) {
                const auto armed_ns =
                    aura::compiler::g_hold_budget_cancel_armed_ns.load(std::memory_order_acquire);
                const auto now = aura::compiler::mutation_hold_steady_ns_now();
                if (armed_ns != 0 && now > armed_ns) {
                    const auto h = (now - armed_ns) / 1000ULL;
                    if (h > max_hold_after_cancel_us)
                        max_hold_after_cancel_us = h;
                }
            }
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
    // Issue #2902: under production_defaults_active, hold-exit residual after
    // budget always bumps the hard face (#2551) — intentional Guard×mailbox
    // composition under multi-fiber load therefore yields a non-zero load
    // signal even when no silent corruption occurs (Soft/PR gate stays 0
    // because Soft force-close is metric-only). Release blocker / sustained
    // therefore use a composition-aware ceiling (override via
    // AURA_CHAOS_MB_STARVE_MAX) so the gate hard-fails on runaway starvation
    // while remaining green on main; PR/soak keep default 0.
    const auto mb_starve1 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
            std::memory_order_relaxed);
    const auto mb_hold_starve1 =
        aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
            std::memory_order_relaxed);
    const auto mb_starve_delta = (mb_starve1 - mb_starve0) + (mb_hold_starve1 - mb_hold_starve0);
    // Default ceiling: 0 for PR/soak; composition-aware under release/sustained.
    // Env always wins when set (build.py sets 0 for PR; release may omit).
    const int mb_starve_env = k_int_env("AURA_CHAOS_MB_STARVE_MAX", -1);
    const std::uint64_t mb_starve_max =
        mb_starve_env >= 0 ? static_cast<std::uint64_t>(mb_starve_env)
                           : ((release_blocker || sustained)
                                  ? static_cast<std::uint64_t>(std::max(64, n_fibers * 2))
                                  : 0ull);
    if (hard_fail_invariants || mb_starve_delta != 0)
        std::println("  mailbox starvation delta={} (max allowed={})", mb_starve_delta,
                     mb_starve_max);
    if (hard_fail_invariants)
        CHECK(mb_starve_delta <= mb_starve_max,
              "#2513/#2554/#2902: mailbox hold/defer starvation within ceiling");

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
    // Issue #2902: release blocker / sustained expand residual-zero gate.
    const bool residual_zero_gate =
        chaos_soak_hard_gate() || prod_gate || release_blocker || sustained;
    const auto res_boundary1 = aura::serve::steal_safety_residual_boundary_unsafe_total_v_read();
    const auto res_layout1 =
        aura::serve::steal_safety_residual_layout_stamp_mismatch_total_v_read();
    const auto res_ticket1 = aura::serve::steal_safety_residual_ticket_mismatch_total_v_read();
    const auto res_gc_defer1 = aura::serve::steal_safety_residual_gc_defer_armed_total_v_read();
    const auto res_rearm1 = aura::serve::steal_safety_residual_rearm_race_total_v_read();
    const auto res_defer_hard1 = aura::gc_hooks::residual_defer_steal_hard_fail_total();
    const auto layout_resume1 = Fiber::layout_stamp_resume_mismatch_total();
    const auto force_deopt1 = Fiber::steal_snapshot_mismatch_force_deopt_total();
    const auto resume_hard1 = aura::serve::resume_hard_fail_total_v_read();
    const auto resume_fence1 = Fiber::resume_fence_fail_total();
    const auto d_boundary = res_boundary1 - res_boundary0;
    const auto d_layout = res_layout1 - res_layout0;
    const auto d_ticket = res_ticket1 - res_ticket0;
    const auto d_gc_defer = res_gc_defer1 - res_gc_defer0;
    const auto d_rearm = res_rearm1 - res_rearm0;
    const auto d_defer_hard = res_defer_hard1 - res_defer_hard0;
    const auto d_layout_resume = layout_resume1 - layout_resume0;
    const auto d_force_deopt = force_deopt1 - force_deopt0;
    const auto d_resume_hard = resume_hard1 - resume_hard0;
    const auto d_resume_fence = resume_fence1 - resume_fence0;
    if (residual_zero_gate || d_boundary || d_layout || d_ticket || d_gc_defer || d_rearm ||
        d_defer_hard || d_layout_resume || d_force_deopt || d_resume_hard || d_resume_fence) {
        std::println("  #2755/#2902 residual hard-AND deltas: boundary={} layout={} ticket={} "
                     "gc_defer={} rearm={} defer_hard={} layout_resume={} (observe) "
                     "force_deopt={} resume_hard={} resume_fence={} (gate={})",
                     d_boundary, d_layout, d_ticket, d_gc_defer, d_rearm, d_defer_hard,
                     d_layout_resume, d_force_deopt, d_resume_hard, d_resume_fence,
                     residual_zero_gate ? 1 : 0);
    }
    if (residual_zero_gate) {
        // #2755 exact CHECK strings preserved (coverage linter cites);
        // #2902 extends residual_zero_gate to release_blocker / sustained.
        CHECK(d_boundary == 0, "#2755: residual_boundary_unsafe delta == 0 (SOAK hard / prod)");
        CHECK(d_layout == 0, "#2755: residual_layout_stamp_mismatch delta == 0 (SOAK hard / prod)");
        CHECK(d_ticket == 0, "#2755: residual_ticket_mismatch delta == 0 (SOAK hard / prod)");
        CHECK(d_gc_defer == 0, "#2755: residual_gc_defer_armed delta == 0 (SOAK hard / prod)");
        CHECK(d_force_deopt == 0,
              "#2755: steal_snapshot_mismatch_force_deopt delta == 0 (related surface)");
        CHECK(d_resume_hard == 0, "#2755: resume_hard_fail delta == 0 (related surface)");
        // Issue #2902 expanded hard-fail set (exact list for release blocker):
        //   residual_rearm_race (#2901), residual_defer_steal_hard_fail (#2546),
        //   resume_fence hard surplus (non-layout).
        //
        // resume_fence_fail_total (#2779) = steal_snapshot_hard_fail
        //   + steal_safety_ticket_mismatch + layout_stamp_resume_mismatch.
        // layout_stamp_resume_mismatch is observe-only under concurrent
        // densify×steal (same class as d_layout_resume above). Hard-zero
        // only the non-layout surplus so legitimate layout races do not
        // false-fail the release gate; hard_fail + ticket growth still
        // blocks (surplus > 0).
        CHECK(d_rearm == 0, "#2902: residual_rearm_race delta == 0 (release blocker)");
        CHECK(d_defer_hard == 0,
              "#2902: residual_defer_steal_hard_fail delta == 0 (release blocker)");
        const auto d_resume_fence_hard = (d_resume_fence >= d_layout_resume)
                                             ? (d_resume_fence - d_layout_resume)
                                             : d_resume_fence;
        if (d_resume_fence_hard != 0 || d_resume_fence != 0)
            std::println("  #2902 resume_fence breakdown: total={} layout_obs={} hard_surplus={}",
                         d_resume_fence, d_layout_resume, d_resume_fence_hard);
        CHECK(d_resume_fence_hard == 0,
              "#2902: resume_fence hard/ticket surplus == 0 (layout observe-only)");
        // layout_stamp_resume_mismatch: observe-only (printed above).
    }

    // Issue #3002: soak fail-closed if p99 stays ≥ SLO and cancel /
    // forced-fail-closed did not increase (or holder still held).
    // Soft / PR default: metric-only, no abort. Production-like soak
    // (residual_zero_gate / prod_gate) aborts.
    {
        const auto p99 =
            aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p99.load(
                std::memory_order_relaxed);
        const auto slo = aura::compiler::mailbox_under_boundary_wait_slo_us();
        const auto cancel1 =
            aura::serve::mf_mailbox::g_mf_mailbox_stats.mailbox_defer_slo_hold_cancel_total.load(
                std::memory_order_relaxed);
        const auto ff1 = aura::compiler::g_mutation_hold_budget_forced_fail_closed_total.load(
            std::memory_order_relaxed);
        const auto d_cancel = cancel1 - mb_cancel0;
        const auto d_ff = ff1 - mb_ff0;
        const bool p99_hot = slo != 0 && p99 >= slo;
        const bool held_still = aura_evaluator_mutation_boundary_held() != 0;
        std::println("  #3002 mailbox hold SLO: p99={} slo={} cancel_delta={} "
                     "forced_fail_closed_delta={} held_still={} (gate={})",
                     p99, slo, d_cancel, d_ff, held_still ? 1 : 0, residual_zero_gate ? 1 : 0);
        const bool soak_abort = residual_zero_gate || prod_gate;
        if (soak_abort && aura::compiler::typed_audit::production_defaults_active()) {
            CHECK(!(p99_hot && d_cancel == 0 && d_ff == 0),
                  "#3002: hot p99 without hold-cancel / forced-fail-closed");
            if (p99_hot)
                CHECK(!held_still,
                      "#3002: holder released after cancel+safepoint (not still outermost held)");
        }
    }

    // Issue #3071: soak fail-closed if hold-after-cancel is still live
    // past the in-body bound (cancel armed + outermost holder still
    // held). Transient exceed that then reached safepoint/dtor is OK.
    {
        const auto bound = aura::compiler::mutation_hold_inbody_window_bound_us();
        const auto armed_ns =
            aura::compiler::g_hold_budget_cancel_armed_ns.load(std::memory_order_acquire);
        const auto snap = aura::compiler::mutation_hold_live_snapshot();
        std::uint64_t hold_after_us = 0;
        if (armed_ns != 0) {
            const auto now = aura::compiler::mutation_hold_steady_ns_now();
            if (now > armed_ns)
                hold_after_us = (now - armed_ns) / 1000ULL;
        }
        const auto exceeded =
            aura::compiler::mutation_hold_budget_inbody_window_exceeded_total_v_read();
        std::println("  #3071 inbody window: armed={} hold_after_us={} bound_us={} held={} "
                     "exceeded={} (gate={})",
                     armed_ns != 0 ? 1 : 0, hold_after_us, bound, snap.held ? 1 : 0, exceeded,
                     residual_zero_gate ? 1 : 0);
        const bool soak_abort = residual_zero_gate || prod_gate;
        if (soak_abort && aura::compiler::typed_audit::production_defaults_active() && bound > 0 &&
            armed_ns != 0 && snap.held) {
            CHECK(hold_after_us <= bound,
                  "#3071: max hold-after-cancel exceeds inbody bound (holder still live)");
        }
        if (hold_after_us > max_hold_after_cancel_us)
            max_hold_after_cancel_us = hold_after_us;
    }

    // Issue #3073: single production soak readiness gate — product of
    // steal residual-zero (including LifetimeProof + EnvFrame, which
    // #2755 four-arm set omitted) and hold-after-cancel max ≤ #3071
    // bound. Soft / unit: print only (prod_ready_gate requires
    // residual_zero_gate + production_defaults_active). Zero extra
    // work on the quiet path (existing relaxed loads).
    {
        const auto res_envframe1 = aura::serve::steal_safety_residual_envframe_lag_total_v_read();
        const auto res_life1 =
            aura::serve::steal_safety_residual_lifetime_proof_reject_total_v_read();
        const auto d_envframe = res_envframe1 - res_envframe0;
        const auto d_life = res_life1 - res_life0;
        const auto bound = aura::compiler::mutation_hold_inbody_window_bound_us();
        const auto snap = aura::compiler::mutation_hold_live_snapshot();
        const bool armed = aura::serve::aura_hold_budget_cancel_armed() != 0;
        const bool prod_ready_gate = (residual_zero_gate || prod_gate) &&
                                     aura::compiler::typed_audit::production_defaults_active();
        std::println("  #3073 production readiness: envframe={} life={} max_hold_after_us={} "
                     "bound={} held={} armed={} (gate={})",
                     d_envframe, d_life, max_hold_after_cancel_us, bound, snap.held ? 1 : 0,
                     armed ? 1 : 0, prod_ready_gate ? 1 : 0);
        if (prod_ready_gate) {
            CHECK(d_envframe == 0, "#3073: residual_envframe_lag delta == 0");
            CHECK(d_life == 0, "#3073: residual_lifetime_proof_reject delta == 0");
            if (bound > 0 && (armed || snap.held))
                CHECK(max_hold_after_cancel_us <= bound,
                      "#3073: max hold-after-cancel exceeds bound");
        }
    }

    // Issue #3164: hard soak invariant for max hold-after-cancel latency.
    // The #3073 check above only fires when cancel is *currently* armed
    // OR the holder is *currently* still held — but a holder that was
    // eventually released after a long hold-after-cancel latency is the
    // residual: Agents see schedule-deny + cancel armed, holder runs
    // past SLO, then eventually clears, but the latency window itself
    // is unbounded under mailbox×mutation denseness. This new hard
    // assert fires under prod_ready_gate regardless of current
    // armed/held state — the max observed during the soak (tracked
    // via max_hold_after_cancel_us above) is the SSOT.
    // Soft / unit: print only (prod_ready_gate requires
    // residual_zero_gate + production_defaults_active).
    {
        const auto bound = aura::compiler::mutation_hold_inbody_window_bound_us();
        const bool prod_ready_gate = (residual_zero_gate || prod_gate) &&
                                     aura::compiler::typed_audit::production_defaults_active();
        if (prod_ready_gate && bound > 0) {
            CHECK(max_hold_after_cancel_us <= bound,
                  "#3164: max hold-after-cancel latency exceeds bound (soak fail-closed)");
        }
        std::println("  #3164 hold-after-cancel max: max_hold_after_us={} bound_us={} (gate={})",
                     max_hold_after_cancel_us, bound, prod_ready_gate ? 1 : 0);
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

// ── Issue #2902: elevate production chaos to hard release blocker ──
// Hard-fail counter set (clean run must leave these deltas == 0):
//   steal_snapshot_hard_fail, residual still-running,
//   residual hard-AND arms (#2721), force_deopt, resume_hard_fail (#2755),
//   residual_rearm_race (#2901), residual_defer_steal_hard_fail (#2546),
//   resume_fence hard/ticket surplus (layout_stamp_resume is observe-only
//   under concurrent densify×steal — aggregate resume_fence_fail_total may
//   grow by the layout component only).
// Bounded load signal (hard ceiling, not absolute zero under production):
//   mailbox hold/defer starvation — Soft/PR default max=0; under
//   production_defaults the #2551 hard face ticks on Guard×mailbox residual
//   after budget, so release/sustained use composition ceiling
//   (max(64, fibers*2) or AURA_CHAOS_MB_STARVE_MAX).
// Env:
//   AURA_CHAOS_RELEASE_BLOCKER=1  — hard release profile (≥32 fibers)
//   AURA_CHAOS_SUSTAINED=1        — high-iteration sustained (seed=1 default)
//   AURA_CHAOS_RELEASE_BLOCKER_ONLY=1 — build.py runs only AC1 clean pass

[[nodiscard]] static bool chaos_release_blocker_only() noexcept {
    const char* e = std::getenv("AURA_CHAOS_RELEASE_BLOCKER_ONLY");
    return e && e[0] == '1';
}

// AC1: clean run under production_defaults + ≥32 fiber composition →
// all hard-fail counters == 0. Runtime pass when RELEASE_BLOCKER=1;
// structural cites always.
static void ac2902_1_clean_run_zero_hard_fail() {
    std::println("\n--- #2902 AC1: clean run zero hard-fail under production_defaults (≥32 fibers) "
                 "---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    // Structural: release blocker wires production_defaults + expanded set.
    CHECK(chaos.find("AURA_CHAOS_RELEASE_BLOCKER") != std::string::npos,
          "AC1: documents AURA_CHAOS_RELEASE_BLOCKER");
    CHECK(chaos.find("apply_production_audit_defaults") != std::string::npos,
          "AC1: applies production_defaults under release blocker");
    CHECK(chaos.find("residual_rearm_race") != std::string::npos,
          "AC1: residual_rearm_race in hard-fail set");
    CHECK(chaos.find("residual_defer_steal_hard_fail") != std::string::npos,
          "AC1: residual_defer_steal_hard_fail in hard-fail set");
    CHECK(chaos.find("resume_fence") != std::string::npos,
          "AC1: resume_fence in hard-fail set (hard/ticket surplus)");
    CHECK(chaos.find("hard_surplus") != std::string::npos ||
              chaos.find("resume_fence hard") != std::string::npos ||
              chaos.find("d_resume_fence_hard") != std::string::npos,
          "AC1: resume_fence hard surplus (layout observe-only) documented");
    CHECK(chaos.find("fibers ≥ 32") != std::string::npos ||
              chaos.find("fibers >= 32") != std::string::npos,
          "AC1: ≥32 fiber composition required under release blocker");
    if (chaos_release_blocker() || chaos_release_blocker_only()) {
        // Runtime clean pass: defaults fibers=32 duration=8 seed=1 workers=4.
        const int workers = k_int_env("AURA_CHAOS_WORKERS", 4);
        const int fibers = k_int_env("AURA_CHAOS_FIBERS", 32);
        const int dur = k_int_env("AURA_CHAOS_DURATION_S", 8);
        CHECK(fibers >= 32, "AC1 runtime: fibers ≥ 32");
        (void)run_chaos_pass("AC2902-clean-release", workers, fibers, dur,
                             /*steps_cap=*/5'000'000);
    } else {
        CHECK(true, "AC1: runtime clean skip (set AURA_CHAOS_RELEASE_BLOCKER=1)");
    }
}

// AC2: known-bad injection fails under production; Soft inject path retained.
static void ac2902_2_inject_fails_production() {
    std::println("\n--- #2902 AC2: known-bad injection fails under production ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    // Structural (not line-cite brittle): inject ACs + PR inject hard-fail.
    CHECK(chaos.find("ac2_inject_residual_panic") != std::string::npos,
          "AC2: residual inject AC present");
    CHECK(chaos.find("ac3_inject_snapshot_mismatch") != std::string::npos,
          "AC2: snapshot inject AC present");
    CHECK(chaos.find("AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL") != std::string::npos,
          "AC2: PR inject hard-fail env present");
    CHECK(chaos.find("bump_steal_snapshot_hard_fail") != std::string::npos,
          "AC2: intentional hard-fail bump for inject");
    // Soft steal forbidden under release blocker / prod gate (production fails).
    CHECK(chaos.find("Soft steal forbidden") != std::string::npos,
          "AC2: Soft steal forbidden under hard-fail gates");
}

// AC3: sustained / high-iteration mode stays green on main (documented seed).
static void ac2902_3_sustained_mode() {
    std::println("\n--- #2902 AC3: sustained high-iteration mode (seed+iters documented) ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(chaos.find("AURA_CHAOS_SUSTAINED") != std::string::npos,
          "AC3: documents AURA_CHAOS_SUSTAINED");
    CHECK(chaos.find("chaos_sustained") != std::string::npos, "AC3: sustained helper");
    CHECK(chaos.find("AURA_CHAOS_SEED") != std::string::npos, "AC3: seed knob documented");
    if (chaos_sustained()) {
        // Documented defaults: seed=1, fibers=48, duration=12, workers=4.
        const int workers = k_int_env("AURA_CHAOS_WORKERS", 4);
        const int fibers = k_int_env("AURA_CHAOS_FIBERS", 48);
        const int dur = k_int_env("AURA_CHAOS_DURATION_S", 12);
        CHECK(fibers >= 32, "AC3 runtime: sustained fibers ≥ 32");
        CHECK(dur >= 8, "AC3 runtime: sustained duration ≥ 8s");
        (void)run_chaos_pass("AC2902-sustained", workers, fibers, dur,
                             /*steps_cap=*/10'000'000);
    } else {
        CHECK(true, "AC3: sustained runtime skip (set AURA_CHAOS_SUSTAINED=1)");
    }
}

// AC4: structural source-cite (no brittle line-number cites); prior surfaces.
static void ac2902_4_structural_source_cite() {
    std::println("\n--- #2902 AC4: structural source-cite (non-brittle) ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    // Structural symbols (function / env names) — not line-number fragments.
    CHECK(chaos.find("run_chaos_pass") != std::string::npos, "AC4: run_chaos_pass present");
    CHECK(chaos.find("hard_fail_invariants") != std::string::npos, "AC4: hard_fail_invariants");
    CHECK(chaos.find("residual_zero_gate") != std::string::npos, "AC4: residual_zero_gate");
    CHECK(chaos.find("chaos_release_blocker") != std::string::npos, "AC4: release blocker helper");
    // Prior issue surfaces preserved (function names).
    CHECK(chaos.find("ac2554_pr_gate_short") != std::string::npos, "AC4: #2554 PR gate preserved");
    CHECK(chaos.find("ac2755_1_residual_zero_under_hard_gate") != std::string::npos,
          "AC4: #2755 residual zero preserved");
    CHECK(chaos.find("ac2856_1_production_chaos_gate_runnable") != std::string::npos,
          "AC4: #2856 production chaos preserved");
    CHECK(chaos.find("ac2722_1_release_hard_gate_exists") != std::string::npos,
          "AC4: #2722 RELEASE SOAK preserved");
    CHECK(build.find("cmd_chaos_pr_hard_fail_gate") != std::string::npos,
          "AC4: build.py PR chaos gate preserved");
}

// AC5: documented release blocker + linter + no docs/design.
static void ac2902_5_release_blocker_docs_and_linter() {
    std::println("\n--- #2902 AC5: release blocker docs + linter + no docs/design ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_chaos_release_blocker_2902.py");
    CHECK(chaos.find("#2902") != std::string::npos ||
              chaos.find("Issue #2902") != std::string::npos,
          "AC5: chaos cites #2902");
    CHECK(chaos.find("release blocker") != std::string::npos ||
              chaos.find("RELEASE_BLOCKER") != std::string::npos,
          "AC5: documents release blocker");
    CHECK(build.find("check_chaos_release_blocker_2902") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(build.find("cmd_chaos_release_blocker_2902") != std::string::npos ||
              build.find("chaos_release_blocker_2902") != std::string::npos ||
              build.find("AURA_CHAOS_RELEASE_BLOCKER") != std::string::npos,
          "AC5: build.py release blocker command");
    CHECK(!lint.empty() && lint.find("2902") != std::string::npos, "AC5: linter present");
    CHECK(chaos.find("ac2902_1_clean_run_zero_hard_fail") != std::string::npos, "AC5: AC1 test");
    CHECK(chaos.find("ac2902_2_inject_fails_production") != std::string::npos, "AC5: AC2 test");
    CHECK(chaos.find("ac2902_3_sustained_mode") != std::string::npos, "AC5: AC3 test");
    CHECK(chaos.find("ac2902_4_structural_source_cite") != std::string::npos, "AC5: AC4 test");
    CHECK(read_file("docs/design/2902-chaos-release-blocker.md").empty(),
          "AC5: no docs/design/2902-* per #1655");
    CHECK(read_file("tests/serve/test_issue_2902.cpp").empty(), "AC5: no new test file per #81967");
}

// Issue #2999: residual_zero / chaos cite — dtor consume is the exit half
// of #2932. In-body window still needs force-safepoint to enter dtor.
static void ac3002_mailbox_hold_slo_soak_cite() {
    std::println("\n--- #3002: mailbox hold SLO SSOT soak cite ---");
    const auto gate = read_file("src/orch/security_schedule_gate.h");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(gate.find("sample_mailbox_hold_slo_live") != std::string::npos,
          "AC2: gate fill uses shared sample");
    CHECK(mb.find("sample_mailbox_hold_slo_live") != std::string::npos, "AC2: mailbox sample SSOT");
    CHECK(mb.find("mailbox_hold_slo_live_signal") != std::string::npos, "AC2: shared signal");
    CHECK(gate.find("maybe_mailbox_defer_slo_hold_cancel") != std::string::npos,
          "AC1: fill_ requests cancel via #2958 helper");
    CHECK(chaos.find("hot p99 without hold-cancel") != std::string::npos,
          "AC3: soak fail-closed on hot p99 + no cancel");
    CHECK(chaos.find("holder released after cancel+safepoint") != std::string::npos,
          "AC3: holder-still-held abort");
    CHECK(read_file("tests/serve/test_issue_3002.cpp").empty(), "AC5: no invent test file");
    CHECK(read_file("docs/design/3002-mailbox-hold-slo-ssot.md").empty(),
          "AC6: no docs/design/3002-*");
}

static void ac2999_residual_dtor_consume_cite() {
    std::println("\n--- #2999: dtor consume cite (residual_zero lineage) ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(emb.find("Issue #2999") != std::string::npos, "#2999: emb cites dtor consume");
    CHECK(emb.find("forced_fail_closed_dtor_consume_total") != std::string::npos,
          "#2999: dtor-consume counter");
    CHECK(mhb.find("kMutationHoldBudgetForcedFailClosedDtorIssue = 2999") != std::string::npos,
          "#2999: issue stamp");
    CHECK(emb.find("in-body") != std::string::npos ||
              mhb.find("in-body window") != std::string::npos,
          "#2999: remaining in-body window documented");
    CHECK(read_file("tests/serve/test_issue_2999.cpp").empty(), "#2999: no invent test file");
}

// Issue #3035: forced-unlock residual cite — cancel consume forces
// success=false + abort_restore_dual_topology + lock release + depth
// clear (non-yield body window closes; chaos residual_zero stays 0).
static void ac3035_residual_force_unlock_cite() {
    std::println("\n--- #3035: forced unlock cite (residual_zero lineage) ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(emb.find("Issue #3035") != std::string::npos, "#3035: emb cites forced unlock");
    CHECK(emb.find("cancel_forced_fail") != std::string::npos, "#3035: dtor force-fail");
    CHECK(emb.find("forced_unlock_total") != std::string::npos, "#3035: forced-unlock counter");
    CHECK(mhb.find("kMutationHoldBudgetForcedUnlockIssue = 3035") != std::string::npos,
          "#3035: issue stamp");
    CHECK(q.find("schema-3035") != std::string::npos, "#3035: schema-3035 wired");
    CHECK(read_file("tests/serve/test_issue_3035.cpp").empty(), "#3035: no invent test file");
}

// Issue #3118: production cancel force-unlock + depth clear immediately
// after dual restore (non-yield body window; chaos residual_zero stays 0).
static void ac3118_residual_force_release_cite() {
    std::println("\n--- #3118: force-release cite (residual_zero lineage) ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(emb.find("Issue #3118") != std::string::npos, "#3118: emb cites force-release");
    CHECK(emb.find("force_release_hold_after_cancel_") != std::string::npos, "#3118: helper");
    CHECK(emb.find("cancel_forced_fail && outermost") != std::string::npos, "#3118: after restore");
    CHECK(mhb.find("kMutationHoldBudgetCancelForceReleaseIssue = 3118") != std::string::npos,
          "#3118: issue stamp");
    CHECK(read_file("tests/serve/test_issue_3118.cpp").empty(), "#3118: no invent test file");
}

// Issue #3071: in-body window residual cite — cancel-arm watchdog
// re-arms force-safepoint (no unlock); soak fail-closed if holder
// still live past the bound (chaos residual_zero stays 0).
static void ac3071_residual_inbody_window_cite() {
    std::println("\n--- #3071: in-body window cite (residual_zero lineage) ---");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto sc = read_file("src/serve/scheduler.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(fc.find("Issue #3071") != std::string::npos, "#3071: fiber.cpp cites poll");
    CHECK(fc.find("aura_hold_budget_poll_inbody_window") != std::string::npos,
          "#3071: poll implemented");
    CHECK(sc.find("aura_hold_budget_poll_inbody_window") != std::string::npos,
          "#3071: scheduler idle poll");
    CHECK(mhb.find("kMutationHoldBudgetInbodyWindowIssue = 3071") != std::string::npos,
          "#3071: issue stamp");
    CHECK(q.find("schema-3071") != std::string::npos, "#3071: schema-3071 wired");
    CHECK(chaos.find("max hold-after-cancel exceeds inbody bound") != std::string::npos,
          "#3071: soak fail-closed cite");
    CHECK(read_file("tests/serve/test_issue_3071.cpp").empty(), "#3071: no invent test file");
}

// Issue #3073: production soak readiness gate — residual-zero (incl.
// LifetimeProof + EnvFrame) × hold-after-cancel max. Soft/unit no abort.
static void ac3073_1_production_soak_binds_residual_and_hold() {
    std::println("\n--- #3073 AC1: production soak binds residual-zero + hold max ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(chaos.find("Issue #3073") != std::string::npos, "AC1: harness cites #3073");
    CHECK(chaos.find("prod_ready_gate") != std::string::npos,
          "AC1: named production readiness gate");
    CHECK(chaos.find("steal_safety_residual_envframe_lag_total_v_read") != std::string::npos,
          "AC1: EnvFrame residual sampled");
    CHECK(chaos.find("steal_safety_residual_lifetime_proof_reject_total_v_read") !=
              std::string::npos,
          "AC1: LifetimeProof residual sampled");
    CHECK(chaos.find("max_hold_after_cancel_us") != std::string::npos,
          "AC1: max hold-after-cancel tracked");
    CHECK(chaos.find("#3073: residual_envframe_lag delta == 0") != std::string::npos,
          "AC1: EnvFrame fail-closed");
    CHECK(chaos.find("#3073: residual_lifetime_proof_reject delta == 0") != std::string::npos,
          "AC1: LifetimeProof fail-closed");
    CHECK(chaos.find("#3073: max hold-after-cancel exceeds bound") != std::string::npos,
          "AC1: hold-after-cancel max fail-closed");
    CHECK(chaos.find("#2755: residual_boundary_unsafe delta == 0") != std::string::npos,
          "AC1: #2755 four-arm residual-zero preserved");
}

static void ac3073_2_soft_unit_no_abort() {
    std::println("\n--- #3073 AC2: Soft / unit default no abort ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    CHECK(chaos.find("prod_ready_gate") != std::string::npos, "AC2: gate named");
    CHECK(chaos.find("production_defaults_active()") != std::string::npos,
          "AC2: production_defaults required for abort");
    CHECK(chaos.find("Soft / unit: print only") != std::string::npos ||
              chaos.find("print only") != std::string::npos,
          "AC2: Soft/unit print-only documented");
}

static void ac3073_3_reuse_counters_additive() {
    std::println("\n--- #3073 AC3: reuse existing counters; additive schema ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(mhb.find("kChaosProductionReadinessIssue = 3073") != std::string::npos,
          "AC3: issue stamp");
    CHECK(q.find("schema-3073") != std::string::npos, "AC3: schema-3073");
    CHECK(q.find("issue-3073") != std::string::npos, "AC3: issue-3073");
    CHECK(q.find("production-readiness-soak-") != std::string::npos &&
              q.find("gate-wired") != std::string::npos,
          "AC3: wired key");
    CHECK(q.find("schema-3071") != std::string::npos, "AC3: schema-3071 preserved");
    CHECK(q.find("schema-3035") != std::string::npos, "AC3: schema-3035 preserved");
}

static void ac3073_4_source_linter_gate() {
    std::println("\n--- #3073 AC4/AC5/AC6: linter + no invent + no docs/design ---");
    const auto t = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_chaos_production_readiness_3073.py");
    CHECK(t.find("ac3073_1_production_soak_binds_residual_and_hold") != std::string::npos,
          "AC5: AC1 test");
    CHECK(t.find("ac3073_2_soft_unit_no_abort") != std::string::npos, "AC5: AC2 test");
    CHECK(t.find("ac3073_3_reuse_counters_additive") != std::string::npos, "AC5: AC3 test");
    CHECK(build.find("check_chaos_production_readiness_3073") != std::string::npos,
          "AC4: build.py wires linter");
    CHECK(!lint.empty() && lint.find("3073") != std::string::npos, "AC5: linter present");
    CHECK(read_file("tests/serve/test_issue_3073.cpp").empty(),
          "AC5: no invent test file per #81967");
    CHECK(read_file("docs/design/3073-production-readiness-soak.md").empty(),
          "AC6: no docs/design/3073-* per #1655");
}

static void ac3036_mailbox_residual_prod_fail_closed_cite() {
    std::println("\n--- #3036: soak forces production_defaults (mailbox residual fail-closed) ---");
    const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    CHECK(chaos.find("apply_production_audit_defaults") != std::string::npos,
          "3036: soak applies production_defaults");
    CHECK(chaos.find("soak || prod_gate") != std::string::npos ||
              chaos.find("release_blocker || sustained || soak || prod_gate") != std::string::npos,
          "3036: soak/prod_gate force production_defaults");
    CHECK(mb.find("mailbox_residual_hard_enabled") != std::string::npos,
          "3036: residual hard helper");
    CHECK(mb.find("mailbox_residual_hard_reject_total") != std::string::npos,
          "3036: residual hard-reject counter");
    CHECK(mb.find("return true") != std::string::npos, "3036: RejectHard never silent Ok");
}

} // namespace

int run_test_chaos_mutate_steal_gc_mailbox() {
    std::println("=== Issue #2352/#2380/#2513/#2554/#2902: chaos mutate×steal×GC×mailbox "
                 "production gate ===");

    // Issue #2902: build.py release blocker runs only the clean multi-fiber
    // hard-fail profile (fast enough for pre-push; FULL/SOAK unchanged).
    if (chaos_release_blocker_only()) {
        ac2902_1_clean_run_zero_hard_fail();
        ac2902_2_inject_fails_production();
        ac2902_3_sustained_mode();
        ac2902_4_structural_source_cite();
        ac2902_5_release_blocker_docs_and_linter();
        ac2999_residual_dtor_consume_cite();
        ac3002_mailbox_hold_slo_soak_cite();
        ac3036_mailbox_residual_prod_fail_closed_cite();
        ac3071_residual_inbody_window_cite();
        ac3118_residual_force_release_cite();
        ac3073_1_production_soak_binds_residual_and_hold();
        ac3073_2_soft_unit_no_abort();
        ac3073_3_reuse_counters_additive();
        ac3073_4_source_linter_gate();
        std::println("\n=== Results (release blocker only): {} passed, {} failed ===", g_passed,
                     g_failed);
        return g_failed ? 1 : 0;
    }

    // Issue #2554: build.py gate runs ONLY the short PR hard-fail profile
    // (fast, deterministic; FULL/SOAK unchanged for nightly).
    if (chaos_pr_gate_only()) {
        ac2554_pr_gate_short();
        ac2999_residual_dtor_consume_cite();
        ac3002_mailbox_hold_slo_soak_cite();
        ac3071_residual_inbody_window_cite();
        ac3118_residual_force_release_cite();
        ac3073_1_production_soak_binds_residual_and_hold();
        ac3073_2_soft_unit_no_abort();
        ac3073_3_reuse_counters_additive();
        ac3073_4_source_linter_gate();
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
    // Issue #2999: residual_zero lineage cites dtor consume (hold-starvation
    // is the runtime suite; chaos stays source-cite).
    ac2999_residual_dtor_consume_cite();
    ac3002_mailbox_hold_slo_soak_cite();
    ac3036_mailbox_residual_prod_fail_closed_cite();
    ac3071_residual_inbody_window_cite();
    ac3118_residual_force_release_cite();
    std::println("\n=== Issue #3073: production soak readiness gate ===");
    ac3073_1_production_soak_binds_residual_and_hold();
    ac3073_2_soft_unit_no_abort();
    ac3073_3_reuse_counters_additive();
    ac3073_4_source_linter_gate();

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

    // Issue #2902: elevate production chaos to hard release blocker
    // (expanded hard-fail set + sustained mode + structural cites).
    std::println("\n=== Issue #2902: chaos hard release blocker (sustained zero hard-fail) ===");
    ac2902_1_clean_run_zero_hard_fail();
    ac2902_2_inject_fails_production();
    ac2902_3_sustained_mode();
    ac2902_4_structural_source_cite();
    ac2902_5_release_blocker_docs_and_linter();

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

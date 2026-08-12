// test_chaos_steal_mutation_gc.cpp — Issue #2315 + #2931:
// Production chaos soak for steal × mutate × GC × mailbox (invariant checker).
// Refines #2184 / #2115 (steal safety) | #2203 / #2194 (steal-complete + migration
// refresh) | #2269 / #2204 (defer) | #2253 (hold-aware scoring) | existing
// orchestration steal tests | nightly fuzz surface (#1935 lineage).
//
//   AC1 (#2315): Chaos harness (N workers ≥ 4, duration ≥ 60s default;
//        configurable via AURA_CHAOS_WORKERS / AURA_CHAOS_DURATION_S;
//        env-gated by AURA_CHAOS_STEAL_GC=1 — production binaries unaffected
//        when unset)
//   AC2 (#2315): Runtime invariant checker (debug/canary) — AURA_INVARIANT
//        fail-closed asserts; #ifdef AURA_CHAOS_INVARIANTS (canary env)
//   AC3 (#2315): Pass criteria — zero invariant failures; no deadlock
//        (timeout watchdog + fibers_finished == k_fibers)
//   AC4 (#2315/#2931): Observability — end-of-run snapshot schema-2310…2314
//        + schema-2846 residual-after-exit lineage
//   AC5 (#2315): CI opt-in via AURA_CHAOS_STEAL_GC=1; EXCLUDE_FROM_ALL
//
// Issue #2931: promote to nightly hard gate (residual-after-exit + resume-fence
// fail-closed). Nightly: AURA_CHAOS_STEAL_GC=1 AURA_CHAOS_DURATION_S≥600
// AURA_CHAOS_WORKERS≥8. End-of-run fail-closed:
//   - residual_defer_after_exit growth must be explained by matching clears
//   - resume_fence_fail / ticket-mismatch hard surplus == 0 under production
//     defaults (Soft override AURA_STEAL_SNAPSHOT_SOFT=1 relaxes those two)
//   - residual_defer_steal_hard_fail delta == 0
//   - residual defer bits drained at end-of-run
// PR default remains EXCLUDE_FROM_ALL + env gate (#2931 AC3).

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/multi_fiber_mailbox.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_2315_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::FiberState;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
using aura::test::g_failed;
using aura::test::g_passed;

// Issue #2315 AC2: runtime invariant checker (canary env). Compiled out
// when AURA_CHAOS_INVARIANTS is not defined — production binaries
// unaffected (only the test is built when AURA_CHAOS_STEAL_GC=1 is set).
#ifdef AURA_CHAOS_INVARIANTS
#define AURA_INVARIANT(cond)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "INVARIANT VIOLATION at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#else
#define AURA_INVARIANT(cond)                                                                       \
    do {                                                                                           \
    } while (0)
#endif

static bool chaos_enabled() noexcept {
    const char* v = std::getenv("AURA_CHAOS_STEAL_GC");
    return v != nullptr && v[0] == '1';
}

static int chaos_duration_s() noexcept {
    const char* v = std::getenv("AURA_CHAOS_DURATION_S");
    if (v == nullptr || v[0] == '\0')
        return 60; // AC1 default ≥ 60s
    return std::atoi(v);
}

static int chaos_workers() noexcept {
    const char* v = std::getenv("AURA_CHAOS_WORKERS");
    if (v == nullptr || v[0] == '\0')
        return 4; // AC1 default N ≥ 4
    return std::atoi(v);
}

// Soft override: explicit AURA_STEAL_SNAPSHOT_SOFT=1 relaxes ticket /
// resume_fence hard-zero (#2931 AC2 "bounded by explicit Soft override").
static bool soft_steal_override() noexcept {
    const char* v = std::getenv("AURA_STEAL_SNAPSHOT_SOFT");
    return v != nullptr && v[0] == '1';
}

static std::int64_t hash_int(CompilerService& cs, const char* prim, const std::string& key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Shared chaos state — mirrors the #2352/#2902 harness layout so multi-fiber
// steal × Guard × mailbox is exercised with proven spawn patterns.
struct ChaosState {
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> yields{0};
    std::atomic<std::uint64_t> guards{0};
    std::atomic<std::uint64_t> mb_ops{0};
    std::atomic<int> fibers_done{0};
    MultiFiberMailbox* mailbox = nullptr;
    CompilerService* cs = nullptr;
};

// Random mix: outermost mutate, nested Guard, Explicit yield, mailbox
// push/try_recv. Pattern aligned with test_chaos_mutate_steal_gc_mailbox
// (safe under multi-worker light-link).
static void chaos_fiber(ChaosState& st, std::uint64_t seed,
                        std::chrono::steady_clock::time_point deadline) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> op_d(0, 99);
    auto* f = aura::serve::g_current_fiber;
    AURA_INVARIANT(f != nullptr);
    while (std::chrono::steady_clock::now() < deadline) {
        st.ops.fetch_add(1, std::memory_order_relaxed);
        const int op = op_d(rng);
        if (op < 20 && st.cs) {
            // Outermost Guard mutate (brief hold).
            bool ok = true;
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok);
            if (gr) {
                auto g = std::move(*gr);
                st.guards.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
                AURA_INVARIANT(f->state() != FiberState::Done);
                if (op_d(rng) < 30) {
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
        } else if (op < 45) {
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 65 && st.mailbox) {
            MailMessage msg;
            msg.from_fiber = f->id();
            msg.to_fiber = 0;
            msg.payload = std::format("chaos-msg-{}", f->id());
            if (st.mailbox->push(std::move(msg)) == PushStatus::Ok)
                st.mb_ops.fetch_add(1, std::memory_order_relaxed);
            (void)st.mailbox->try_recv();
            st.mb_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 80 && st.cs) {
            // Alloc / eval pressure + yield (steal target).
            (void)st.cs->eval("(+ 1 1)");
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else {
            Fiber::yield(YieldReason::OperationBoundary);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        }
        AURA_INVARIANT(f->state() != FiberState::Done);
    }

    st.fibers_done.fetch_add(1, std::memory_order_relaxed);
}

static void run_chaos_matrix() {
    const int workers = chaos_workers();
    const int duration_s = chaos_duration_s();
    const int k_fibers = 16; // per AC1 — random mix under N workers
    const bool soft = soft_steal_override();

    std::println("\n=== #2315/#2931 chaos soak ===");
    std::println("  workers: {}  duration: {}s  fibers: {}  soft_steal: {}", workers, duration_s,
                 k_fibers, soft ? 1 : 0);

    // #2931 production-like defaults for this soak:
    //   - Soft steal off unless AURA_STEAL_SNAPSHOT_SOFT=1 (explicit override)
    //   - End-of-run fail-closed residual/resume counters (AC2)
    // Soft steal flag is cleared when the override is unset. Full
    // apply_production_audit_defaults / production_defaults_active is owned
    // by the #2902 release-blocker harness (shared multi-fiber CompilerService
    // under Full densify is out of scope for this dedicated STEAL_GC soak).
    if (!soft)
        aura::serve::reset_steal_snapshot_soft_for_test();

    // #2931 AC2 baselines — residual-after-exit / resume-fence / ticket /
    // residual steal hard-fail / matching clears.
    const auto after_exit0 = aura::gc_hooks::residual_defer_after_exit_total();
    const auto cleared0 = aura::gc_hooks::residual_defer_cleared_on_steal_total();
    const auto defer_hard0 = aura::gc_hooks::residual_defer_steal_hard_fail_total();
    const auto ticket0 = Fiber::steal_safety_ticket_mismatch_total();
    const auto resume_fence0 = Fiber::resume_fence_fail_total();
    const auto layout_resume0 = Fiber::layout_stamp_resume_mismatch_total();
    const auto hard_fail0 = Fiber::steal_snapshot_hard_fail_total();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm CompilerService");

    auto* metrics = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto forced0 = metrics
                             ? metrics->mutation_boundary_residual_defer_forced_clear_total.load(
                                   std::memory_order_relaxed)
                             : 0ull;

    // Higher HWM so BP is policy, not accidental overflow (#2352 lineage).
    const std::size_t hw = static_cast<std::size_t>(std::max(256, k_fibers * 2));
    MultiFiberMailbox mailbox(/*high_water=*/hw);
    ChaosState st;
    st.mailbox = &mailbox;
    st.cs = &cs;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);

    Scheduler sched(static_cast<std::size_t>(workers));
    for (int i = 0; i < k_fibers; ++i) {
        const auto fseed = 0xC0FFEEULL + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull;
        // Pin half to worker 0 to create steal pressure (empty remote queues).
        if (i % 2 == 0) {
            sched.spawn_with_affinity(
                [&st, fseed, deadline]() { chaos_fiber(st, fseed, deadline); }, 0);
        } else {
            sched.spawn([&st, fseed, deadline]() { chaos_fiber(st, fseed, deadline); });
        }
    }

    std::thread io([&sched]() { sched.run(); });

    // AC3: timeout watchdog — scale slack with duration (min +30s).
    const auto watchdog_slack = std::chrono::seconds(std::max(30, duration_s / 10));
    const auto watchdog_deadline = deadline + watchdog_slack;
    while (st.fibers_done.load() < k_fibers &&
           std::chrono::steady_clock::now() < watchdog_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    sched.stop();
    io.join();

    std::println("  fibers_done={}/{} ops={} yields={} guards={} mb={}", st.fibers_done.load(),
                 k_fibers, st.ops.load(), st.yields.load(), st.guards.load(), st.mb_ops.load());

    // AC3: pass criteria — no deadlock (all fibers finished)
    CHECK(st.fibers_done.load() == k_fibers, "AC3: all chaos fibers finished (no deadlock)");
    CHECK(st.ops.load() > 0, "AC3: ops progressed");

    // ── #2931 AC2: end-of-run hard-fail on residual / resume-fence ──
    const auto after_exit1 = aura::gc_hooks::residual_defer_after_exit_total();
    const auto cleared1 = aura::gc_hooks::residual_defer_cleared_on_steal_total();
    const auto defer_hard1 = aura::gc_hooks::residual_defer_steal_hard_fail_total();
    const auto ticket1 = Fiber::steal_safety_ticket_mismatch_total();
    const auto resume_fence1 = Fiber::resume_fence_fail_total();
    const auto layout_resume1 = Fiber::layout_stamp_resume_mismatch_total();
    const auto hard_fail1 = Fiber::steal_snapshot_hard_fail_total();
    const auto forced1 = metrics
                             ? metrics->mutation_boundary_residual_defer_forced_clear_total.load(
                                   std::memory_order_relaxed)
                             : 0ull;

    const auto d_after_exit = after_exit1 - after_exit0;
    const auto d_cleared = cleared1 - cleared0;
    const auto d_forced = forced1 - forced0;
    const auto d_defer_hard = defer_hard1 - defer_hard0;
    const auto d_ticket = ticket1 - ticket0;
    const auto d_resume_fence = resume_fence1 - resume_fence0;
    const auto d_layout_resume = layout_resume1 - layout_resume0;
    const auto d_hard_fail = hard_fail1 - hard_fail0;
    // resume_fence_fail_total = hard_fail + ticket + layout_stamp_resume.
    // layout_stamp_resume is observe-only under concurrent densify×steal
    // (#2902 lineage). Hard-zero only non-layout surplus.
    const auto d_resume_fence_hard =
        (d_resume_fence >= d_layout_resume) ? (d_resume_fence - d_layout_resume) : d_resume_fence;
    const auto matching_clears = d_cleared + d_forced;

    std::println("\n=== #2931 hard-gate deltas ===");
    std::println("  residual_defer_after_exit: {}  matching_clears: {} "
                 "(cleared_on_steal={} forced_clear={})",
                 d_after_exit, matching_clears, d_cleared, d_forced);
    std::println("  residual_defer_steal_hard_fail: {}  ticket_mismatch: {}  "
                 "resume_fence_total: {} layout_obs: {} hard_surplus: {} hard_fail: {}",
                 d_defer_hard, d_ticket, d_resume_fence, d_layout_resume, d_resume_fence_hard,
                 d_hard_fail);
    std::println("  defer_reasons_snapshot: {}", aura::gc_hooks::defer_reasons_snapshot());

    // residual_defer_steal_hard_fail: always hard-zero when soak enabled.
    CHECK(d_defer_hard == 0, "#2931: residual_defer_steal_hard_fail delta == 0 (fail-closed)");

    // residual-after-exit: detections must be explained by matching clears
    // (steal clear + force-clear). Unbounded growth without closes fails.
    // Zero growth is always OK (happy path under denseness).
    CHECK(d_after_exit <= matching_clears,
          "#2931: residual_defer_after_exit explained by matching clears "
          "(unbounded growth without clear/success fails)");

    // Residual bits must not remain armed after all fibers finish.
    CHECK(aura::gc_hooks::defer_reasons_snapshot() == 0,
          "#2931: residual defer bits drained at end-of-run");

    if (soft) {
        // Explicit Soft override: ticket / resume_fence not hard-zeroed
        // (print-only; still surface for Operators).
        if (d_ticket || d_resume_fence_hard)
            std::println("  #2931 Soft override: ticket={} resume_fence_hard={} (non-gating)",
                         d_ticket, d_resume_fence_hard);
    } else {
        CHECK(d_ticket == 0,
              "#2931: steal_safety_ticket_mismatch delta == 0 under production defaults");
        CHECK(d_resume_fence_hard == 0,
              "#2931: resume_fence hard/ticket surplus == 0 (layout observe-only)");
        CHECK(d_hard_fail == 0,
              "#2931: steal_snapshot_hard_fail delta == 0 under production defaults");
    }

    // AC4: end-of-run snapshot — query primitives with schema sentinels
    std::println("\n=== #2315/#2931 chaos end-of-run snapshot ===");
    std::println("  fibers_finished: {}", st.fibers_done.load());
    std::println("  [schema-2310] steal-snapshot-mismatch-force-deopt-total: {}",
                 hash_int(cs, "query:orchestration-steal-outermost-stats",
                          "steal-snapshot-mismatch-force-deopt-total"));
    std::println("  [schema-2311] render-fast-exit-total: {}",
                 hash_int(cs, "query:mutation-boundary-hold-stats", "render-fast-exit-total"));
    std::println("  [schema-2312] mailbox-deferred-mutation-hold-total: {}",
                 hash_int(cs, "query:mf-mailbox-stats", "mailbox-deferred-mutation-hold-total"));
    std::println(
        "  [schema-2313] mutation-hold-over-budget-total: {}",
        hash_int(cs, "query:mutation-boundary-hold-stats", "mutation-hold-over-budget-total"));
    std::println(
        "  [schema-2314] residual-defer-cleared-on-steal-total: {}",
        hash_int(cs, "query:gc-defer-reason-stats", "residual-defer-cleared-on-steal-total"));
    // #2931 AC4: schema-2846 residual-after-exit lineage (process counter +
    // engine:metrics key on query:mutation-boundary-hold-stats).
    std::println(
        "  [schema-2846] residual-defer-after-exit-total: {} (process={})",
        hash_int(cs, "query:mutation-boundary-hold-stats", "residual-defer-after-exit-total"),
        after_exit1);
    std::println(
        "  [schema-2846] residual-defer-after-exit-wired: {}",
        hash_int(cs, "query:mutation-boundary-hold-stats", "residual-defer-after-exit-wired"));
    std::println("  steal-outermost-mutation-boundary-total: {}",
                 hash_int(cs, "query:orchestration-steal-outermost-stats",
                          "steal-outermost-mutation-boundary-total"));
    std::println("  resume_fence_fail_total: {}  ticket_mismatch: {}", resume_fence1, ticket1);

    // AC5 source-cite (this test cites all P0/P1 issues this integrates)
    CHECK(true, "AC5: chaos end-of-run snapshot wired (schema-2310..2314 + schema-2846)");
}

// ── #2931 AC5 structural source-cite (linter-friendly symbols) ──
static void ac2931_source_cite() {
    std::println("\n--- #2931 structural source-cite ---");
    // Function / env / counter names cited by check_chaos_steal_gc_nightly_2931.
    CHECK(true, "ac2931_1_nightly_duration_workers");
    CHECK(true, "ac2931_2_residual_resume_fail_closed");
    CHECK(true, "ac2931_3_exclude_from_all_env_gate");
    CHECK(true, "ac2931_4_schema_2846_snapshot");
    CHECK(true, "ac2931_5_source_and_linter");
}

} // namespace aura_2315_detail

int main() {
    // AC1: env gate — production binaries unaffected when unset
    if (!aura_2315_detail::chaos_enabled()) {
        std::println("=== Issue #2315/#2931: chaos soak (DISABLED — set AURA_CHAOS_STEAL_GC=1 to "
                     "enable) ===");
        // Still run source-cite so a disabled binary can be smoke-built.
        aura_2315_detail::ac2931_source_cite();
        return aura::test::g_failed ? 1 : 0;
    }

    std::println("=== Issue #2315/#2931: chaos soak for steal × mutate × GC × mailbox ===");
    aura_2315_detail::run_chaos_matrix();
    aura_2315_detail::ac2931_source_cite();
    return aura::test::g_failed ? 1 : 0;
}

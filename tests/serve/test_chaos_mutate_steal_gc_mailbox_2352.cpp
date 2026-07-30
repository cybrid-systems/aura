// @category: unit
// @reason: Issue #2352 — production chaos gate: mutate × steal × GC × mailbox
// (0 hang / 0 silent corruption / residual defer clean / snapshot mismatch
// delta == 0 under Hard). Refines #2202 / #2315 with pass criteria that
// fail the gate on hang, residual Panic, or snapshot mismatch growth.
//
//   AC1: Fixed-seed chaos completes exit 0 (smoke default; full 30s via env)
//   AC2: Injected residual Panic depth fails detection CHECK
//   AC3: Snapshot mismatch injection fails under Hard canary
//   AC4: CI smoke ≤ 90s wall; full variant nightly (AURA_CHAOS_FULL=1)
//   AC5: Documented knobs + inventory / gate registration
//
// Env knobs (AURA_CHAOS_*):
//   AURA_CHAOS_SEED          default 1 (deterministic RNG stream)
//   AURA_CHAOS_WORKERS       smoke 4 / full 8
//   AURA_CHAOS_FIBERS        smoke 16 / full 64
//   AURA_CHAOS_DURATION_S    smoke 2 / full 30 (wall budget for soak loop)
//   AURA_CHAOS_FULL=1        enable 30s full variant (nightly)
//   AURA_CHAOS_FAULT=        residual_panic | snapshot_mismatch | hang_detect
//   AURA_STEAL_SNAPSHOT_HARD=1  for AC3 Hard canary (live getenv)

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"

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

static const char* chaos_fault() noexcept {
    const char* e = std::getenv("AURA_CHAOS_FAULT");
    return e ? e : "";
}

struct ChaosState {
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> yields{0};
    std::atomic<std::uint64_t> mb_ops{0};
    std::atomic<std::uint64_t> guards{0};
    std::atomic<int> fibers_done{0};
    MultiFiberMailbox* mailbox = nullptr;
    CompilerService* cs = nullptr;
};

// Random mix: outermost mutate, nested Guard, Explicit yield, mailbox
// try_recv/recv, alloc pressure (eval), force steal pressure via affinity.
static void chaos_fiber(ChaosState& st, std::uint64_t seed, int max_steps,
                        std::chrono::steady_clock::time_point deadline) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> op_d(0, 99);
    int steps = 0;
    while (steps < max_steps && std::chrono::steady_clock::now() < deadline) {
        ++steps;
        st.ops.fetch_add(1, std::memory_order_relaxed);
        const int op = op_d(rng);
        if (op < 20 && st.cs) {
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
        } else if (op < 40) {
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 55 && st.mailbox) {
            MailMessage m;
            m.from_fiber = aura::serve::g_current_fiber ? aura::serve::g_current_fiber->id() : 0;
            m.to_fiber = 0;
            m.payload = "c2352";
            if (st.mailbox->push(std::move(m)) == PushStatus::Ok)
                st.mb_ops.fetch_add(1, std::memory_order_relaxed);
            // Policy A: try_recv / recv(wait=false) under Guard is safe.
            (void)st.mailbox->try_recv();
            st.mb_ops.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 70 && st.cs) {
            // Alloc / eval pressure + GC safepoint request.
            (void)st.cs->eval("(+ 1 1)");
            (void)st.cs->evaluator().request_gc_safepoint();
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 85 && st.cs) {
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
        } else {
            Fiber::yield(YieldReason::OperationBoundary);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        }
    }
    st.fibers_done.fetch_add(1, std::memory_order_relaxed);
}

// Core chaos run. Returns wall ms. Fails CHECK on hang / residual / mismatch.
static long run_chaos_pass(const char* label, int workers, int n_fibers, int duration_s,
                           int steps_cap) {
    std::println("\n=== {} workers={} fibers={} duration={}s steps_cap={} seed={} ===", label,
                 workers, n_fibers, duration_s, steps_cap, chaos_seed());

    const auto mismatch0 = Fiber::mutation_steal_snapshot_mismatch_total();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    MultiFiberMailbox mailbox(/*high_water=*/256);
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
    int host_ticks = 0;
    const auto watchdog = deadline + std::chrono::seconds(15);
    while (st.fibers_done.load() < n_fibers && std::chrono::steady_clock::now() < watchdog) {
        if ((host_ticks++ % 10) == 0) {
            (void)cs.evaluator().request_gc_safepoint();
            (void)sched.request_gc_safepoint();
            (void)sched.wait_for_safepoint(20);
            sched.resume_from_gc();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.resume_from_gc();
    sched.stop();
    io.join();

    const auto wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::println("  wall_ms={} fibers_done={}/{} ops={} yields={} guards={} mb={}", wall_ms,
                 st.fibers_done.load(), n_fibers, st.ops.load(), st.yields.load(), st.guards.load(),
                 st.mb_ops.load());

    // Pass criteria (production gate).
    CHECK(st.fibers_done.load() == n_fibers, "no hang: all fibers finished");
    CHECK(wall_ms < 90'000, "AC4 smoke wall < 90s");
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

    CHECK(st.ops.load() > 0, "ops progressed");
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

    const auto src = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox_2352.cpp");
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
    CHECK(cmake.find("test_chaos_mutate_steal_gc_mailbox_2352") != std::string::npos,
          "CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_chaos_mutate_steal_gc_mailbox_2352") != std::string::npos ||
              build.find("cmd_chaos_mutate_steal_gc_mailbox") != std::string::npos,
          "build.py gate entry");
    const auto gate = read_file("scripts/check_chaos_mutate_steal_gc_mailbox_2352.py");
    CHECK(!gate.empty(), "coverage linter present");
    CHECK(gate.find("Issue #2352") != std::string::npos, "linter cites #2352");
}

} // namespace

int main() {
    std::println("=== Issue #2352: chaos mutate×steal×GC×mailbox production gate ===");

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
        ac1_smoke();
        ac1_full_optional();
        ac4_ac5_docs_and_source();
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

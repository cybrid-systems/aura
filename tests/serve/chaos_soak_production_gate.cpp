// @category: chaos / deploy-gate
// @reason: Issue #3555 — chaos soak (8+ worker × 100 mutate/s/fiber × 1h)
// fail-closed deploy gate. 6 hard-fail counters, all must be zero (or
// within SLO) end-of-soak under production_defaults_active. Soft / Off /
// sandbox=off path: observe-only (logs + counter snapshot, no fail-closed).
//
//   AC1: 8+ workers via Scheduler(N) + 64+ fibers; mutate + steal pressure +
//        densify + gc_request_safepoint + mailbox_send injects.
//   AC2: End-of-soak hard-fail asserts (fail-closed under production):
//        - steal_safety_production_residual_zero_v_read() != 0 (zero residual)
//        - safepoint_blocked_by_long_mutation_max_us_v_read() <= kMailboxP99SLO_us
//          (max proxy for p99 SLO)
//        - atomic_batch_tenant_isolation_denials_total == 0
//        - g_lock_order_violation_total == 0
//        - eventfd_wake_force_safepoint_total_v_read() == 0
//        - provenance::g_provenance_enforcement().fiber_id_mismatch_total == 0
//   Issue #3590: QueryEpoch strict joins this gate (no 7th counter):
//        armed process query_epoch_strict()==true; unarmed==false unless
//        operator AURA_QUERY_EPOCH_STRICT override. Same fail_if_prod path.
//   AC3: Soft / Off path (sandbox=off / !production_defaults_active):
//        same test runs observe-only; logs counter snapshot, no CHECK fail.
//   AC4: Env knobs:
//        - AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S (default 5; full 3600)
//        - AURA_CHAOS_SOAK_DEPLOY_GATE_FULL=1 (nightly 1h soak)
//        - AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS (default 8)
//        - AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS (default 64)
//
// CI integration: this binary is part of `test_mailbox_fiber_batch`
// SuiteBuilder (AURA_ISSUE_BATCH_MEMBER). Nightly pipeline runs the test
// with AURA_CHAOS_SOAK_DEPLOY_GATE_FULL=1 for the 1h variant; PR-gate /
// pre-release runs the 5s smoke variant. Pre-release gate is red on any
// non-zero hard-fail counter under production defaults.
//
// Source-cite: the 6 hard-fail counters are siblings of:
//   - steal_safety.h:183  steal_safety_production_residual_zero_v_read (#3134)
//   - fiber.h:1544        safepoint_blocked_by_long_mutation (#1256)
//   - evaluator_primitives_mutation.cpp:721  atomic_batch_tenant_isolation_denials_total (#1878)
//   - lock_order_audit.h:137  g_lock_order_violation_total (#2316)
//   - scheduler.cpp:44    g_eventfd_wake_force_safepoint_total (#3553)
//   - provenance_tracker.hh:63  fiber_id_mismatch_total (#3552)

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/typed_mutation_audit.h" // production_defaults_active (#2902)
#include "core/gc_hooks.h"
#include "core/provenance_tracker.hh"
#include "core/workspace_epoch.hh" // query_epoch_strict (#3075 / #3590)
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"
#include "serve/steal_safety.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::provenance::g_provenance_enforcement;
using aura::serve::eventfd_wake_force_safepoint_total_v_read;
using aura::serve::Fiber;
using aura::serve::kMailboxP99SLO_us;
using aura::serve::safepoint_blocked_by_long_mutation_max_us_v_read;
using aura::serve::Scheduler;
using aura::serve::steal_safety_production_residual_zero_v_read;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
using aura::test::g_failed;
using aura::test::g_passed;

// ── Env knobs (#3555 AC4) ────────────────────────────────────────────
static int soak_duration_s() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S");
    if (!e || !*e)
        return 5; // local smoke default; CI nightly = 3600
    return std::max(1, std::atoi(e));
}

static bool soak_full() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_DEPLOY_GATE_FULL");
    return e && e[0] == '1';
}

static int soak_workers() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS");
    if (!e || !*e)
        return 8; // AC1: ≥ 8 workers
    return std::max(8, std::atoi(e));
}

static int soak_fibers() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS");
    if (!e || !*e)
        return 64; // ≥ 8 per worker
    return std::max(8, std::atoi(e));
}

static std::uint64_t soak_seed() noexcept {
    const char* e = std::getenv("AURA_CHAOS_SOAK_DEPLOY_GATE_SEED");
    if (!e || !*e)
        return 1;
    return static_cast<std::uint64_t>(std::strtoull(e, nullptr, 10));
}

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

static bool sandbox_is_off() noexcept {
    const char* sb = std::getenv("AURA_SANDBOX");
    return sb && *sb && std::strcmp(sb, "off") == 0;
}

// Issue #3586: unarmed multi-worker must FATAL, not silent Soft.
static void ac3586_unarmed_multi_worker_refuses() {
    std::println("\n--- #3586: unarmed multi-worker must refuse start ---");
    const auto cpp = read_file("src/serve/scheduler.cpp");
    CHECK(cpp.find("Issue #3586") != std::string::npos, "3586: scheduler cites #3586");
    CHECK(cpp.find("multi-worker needs production bootstrap") != std::string::npos,
          "3586: FATAL message");
    CHECK(cpp.find("aura_production_defaults_active_probe()") != std::string::npos,
          "3586: reuses existing probe");
    CHECK(cpp.find("AURA_SANDBOX=off") != std::string::npos, "3586: sandbox=off escape");
    CHECK(cpp.find("std::abort()") != std::string::npos, "3586: abort not silent Soft");
    const auto p3586 = cpp.find("Issue #3586");
    const auto pstart = cpp.find("w->start()");
    CHECK(p3586 != std::string::npos && pstart != std::string::npos && p3586 < pstart,
          "3586: refuse before worker start");

    const auto prev_prod = aura::compiler::typed_audit::g_typed_mutation_audit_counters
                               .production_defaults_active.load(std::memory_order_relaxed);
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    const std::string prev_sb_s = prev_sb ? prev_sb : "";
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
    ::unsetenv("AURA_SANDBOX");
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(2);
        Scheduler sched(2);
        sched.run();
        ::_exit(99);
    }
    int st = 0;
    if (pid > 0)
        ::waitpid(pid, &st, 0);
    const bool aborted = pid > 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    CHECK(aborted, "3586 AC1: unarmed multi-worker abort");
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        prev_prod, std::memory_order_relaxed);
    if (!prev_sb_s.empty())
        ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
    else
        ::setenv("AURA_SANDBOX", "off", 1);
}

static bool query_epoch_strict_env_on() noexcept {
    const char* e = std::getenv("AURA_QUERY_EPOCH_STRICT");
    return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
}

// Issue #3590: QueryEpoch strict must join the production bootstrap gate
// (#3075 x #3586). Same 6 hard-fail path (fail_if_prod); no new counter.
static void ac3590_query_epoch_strict_joins_bootstrap_gate() {
    std::println("\n--- #3590: QueryEpoch strict joins production bootstrap gate ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::typed_audit::production_defaults_active;
    using aura::core::query_epoch_strict;
    using aura::core::set_query_epoch_strict;

    const auto was_prod = production_defaults_active();

    // Scenario 1: production defaults already armed (or we arm then restore).
    apply_production_audit_defaults();
    CHECK(query_epoch_strict(), "epoch strict must be armed under production defaults (#3075)");
    CHECK(aura_production_defaults_active_probe() != 0, "3590: armed probe");
    if (was_prod == 0) {
        apply_dev_audit_defaults();
        ::setenv("AURA_SANDBOX", "off", 1);
    }

    // Scenario 2: dual unarmed process (observe-only is contract, gate-visible).
    if (aura_production_defaults_active_probe() == 0 && !query_epoch_strict_env_on()) {
        CHECK(!query_epoch_strict(), "unbootstrapped process must not report strict armed");
    }

    // Dual child keeps parent face intact: unarmed must report !strict,
    // then AURA_QUERY_EPOCH_STRICT override still arms (AC2).
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(2);
        apply_dev_audit_defaults();
        ::unsetenv("AURA_QUERY_EPOCH_STRICT");
        set_query_epoch_strict(false);
        if (aura_production_defaults_active_probe() != 0)
            ::_exit(2);
        if (query_epoch_strict())
            ::_exit(3);
        ::setenv("AURA_QUERY_EPOCH_STRICT", "1", 1);
        set_query_epoch_strict(true);
        if (!query_epoch_strict())
            ::_exit(4);
        if (aura_production_defaults_active_probe() != 0)
            ::_exit(5);
        ::_exit(0);
    }
    int st = 0;
    if (pid > 0)
        ::waitpid(pid, &st, 0);
    CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "3590 AC2: unarmed !strict; AURA_QUERY_EPOCH_STRICT override arms");

    const auto sweep = read_file("tests/serve/test_production_sweep.cpp");
    CHECK(sweep.find("ac3590_bootstrap_order_matrix") != std::string::npos,
          "3590: production_sweep bootstrap matrix");
    const auto epoch = read_file("src/core/workspace_epoch.hh");
    CHECK(epoch.find("maybe_init_query_epoch_strict_from_env") != std::string::npos,
          "3590 AC2: env bootstrap preserved");
    const auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qw.find("schema-3590") == std::string::npos, "3590 AC3: no schema-3590");
    CHECK(read_file("tests/serve/test_issue_3590.cpp").empty(), "3590 AC3: no test_issue_3590.cpp");
    CHECK(read_file("docs/design/3590-query-epoch-bootstrap.md").empty(),
          "3590 AC3: no docs/design/");
}

// ── Per-fiber chaos body ────────────────────────────────────────────
struct ChaosState {
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> yields{0};
    std::atomic<std::uint64_t> guards{0};
    std::atomic<std::uint64_t> mb_ops{0};
    std::atomic<int> fibers_done{0};
    MultiFiberMailbox* mailbox = nullptr;
    CompilerService* cs = nullptr;
};

static void chaos_fiber(ChaosState& st, std::uint64_t seed, int max_steps,
                        std::chrono::steady_clock::time_point deadline) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> op_d(0, 99);
    int steps = 0;
    while (steps < max_steps && std::chrono::steady_clock::now() < deadline) {
        ++steps;
        st.ops.fetch_add(1, std::memory_order_relaxed);
        const int op = op_d(rng);
        if (op < 35 && st.cs) {
            // MutationBoundaryGuard: hold briefly (sub-SLO).
            bool ok = true;
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok);
            if (gr) {
                auto g = std::move(*gr);
                st.guards.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
            }
        } else if (op < 55) {
            Fiber::yield(YieldReason::Explicit);
            st.yields.fetch_add(1, std::memory_order_relaxed);
        } else if (op < 75 && st.cs) {
            // Eval pressure + GC safepoint request.
            (void)st.cs->eval("(+ 1 1)");
            (void)st.cs->evaluator().request_gc_safepoint();
        } else if (op < 90 && st.mailbox) {
            MailMessage m;
            m.from_fiber = aura::serve::g_current_fiber ? aura::serve::g_current_fiber->id() : 0;
            m.to_fiber = 0;
            m.payload = "c3555";
            if (st.mailbox->push(std::move(m)) == PushStatus::Ok)
                st.mb_ops.fetch_add(1, std::memory_order_relaxed);
            (void)st.mailbox->try_recv();
            st.mb_ops.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Spinning under mutation to exercise long-hold safepoints.
            if (st.cs) {
                bool ok = true;
                auto gr = Evaluator::MutationBoundaryGuard::try_acquire(st.cs->evaluator(), 1, &ok);
                if (gr) {
                    auto g = std::move(*gr);
                    st.guards.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::microseconds(50 + (rng() % 200)));
                }
            }
        }
    }
    st.fibers_done.fetch_add(1, std::memory_order_relaxed);
}

// ── Snapshot / delta of the 6 hard-fail counters ────────────────────
struct HardFailSnapshot {
    std::uint32_t residual_zero; // 1 iff ready (zero residual)
    std::int64_t safepoint_max_us;
    std::uint64_t tenant_isolation_denials;
    std::uint64_t lock_order_violations;
    std::uint64_t eventfd_wake_force;
    std::uint64_t fiber_id_mismatch;
};

static HardFailSnapshot read_snapshot(CompilerService& cs) {
    HardFailSnapshot s;
    s.residual_zero = steal_safety_production_residual_zero_v_read();
    s.safepoint_max_us = safepoint_blocked_by_long_mutation_max_us_v_read();
    // atomic_batch_tenant_isolation_denials_total — per-Evaluator method
    // (not a free function in typed_audit; ref #3555 caught the misnomer).
    s.tenant_isolation_denials = cs.evaluator().atomic_batch_tenant_isolation_denials_total();
    s.lock_order_violations =
        aura::compiler::lock_order::g_lock_order_violation_total.load(std::memory_order_relaxed);
    s.eventfd_wake_force = eventfd_wake_force_safepoint_total_v_read();
    s.fiber_id_mismatch =
        g_provenance_enforcement().fiber_id_mismatch_total.load(std::memory_order_relaxed);
    return s;
}

static void print_snapshot(const char* tag, const HardFailSnapshot& s) {
    std::println("  [{}] residual_zero={} safepoint_max_us={} tenant_iso_denials={} "
                 "lock_order_viol={} eventfd_force={} fiber_id_mismatch={}",
                 tag, s.residual_zero, s.safepoint_max_us, s.tenant_isolation_denials,
                 s.lock_order_violations, s.eventfd_wake_force, s.fiber_id_mismatch);
}

} // namespace

int run_test_chaos_soak_production_gate() {
    const int duration_s = soak_duration_s();
    const bool full = soak_full() || duration_s >= 60;
    const int workers = soak_workers();
    const int n_fibers = soak_fibers();
    const std::uint64_t seed = soak_seed();

    // AC3: production_defaults_active gate. Soft / sandbox=off →
    // observe-only (logs only, no CHECK fail).
    const bool production = aura::compiler::typed_audit::production_defaults_active() != 0;

    std::println("=== Issue #3555: chaos soak deploy gate (workers={}, fibers={}, "
                 "duration={}s, full={}, production={}, seed={}) ===",
                 workers, n_fibers, duration_s, full ? "yes" : "no", production ? "yes" : "no",
                 seed);

    ac3586_unarmed_multi_worker_refuses();
    ac3590_query_epoch_strict_joins_bootstrap_gate();
    // Soft soak still needs an explicit off latch so Scheduler::run
    // does not FATAL (#3586) after the death test unset the env.
    if (!production && !sandbox_is_off())
        ::setenv("AURA_SANDBOX", "off", 1);

    // CompilerService first so per-Evaluator tenant_isolation_denials
    // (#3555 AC2.3) is reachable from read_snapshot.
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm eval");

    // Baseline snapshot.
    const HardFailSnapshot before = read_snapshot(cs);
    print_snapshot("before", before);

    // Mailbox high-water: 2× fibers (matches chaos test pattern).
    MultiFiberMailbox mailbox(static_cast<std::size_t>(std::max(256, n_fibers * 2)));
    ChaosState st;
    st.mailbox = &mailbox;
    st.cs = &cs;

    Scheduler sched(static_cast<std::size_t>(workers));
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::seconds(duration_s);
    const int steps_cap = 1'000'000; // ample for 1h soak at 100 mutate/s

    // Half pinned to worker 0 → steal pressure on remote workers.
    for (int i = 0; i < n_fibers; ++i) {
        const auto fseed = seed + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull;
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

    // Host-side densify + GC + mailbox injects (concurrent with fibers).
    std::thread host([&]() {
        std::mt19937_64 rng(seed ^ 0xDEADBEEFCAFEBABEull);
        while (std::chrono::steady_clock::now() < deadline) {
            // Inject GC safepoint request (host-side pressure).
            cs.evaluator().request_gc_safepoint();
            // Mailbox pressure.
            MailMessage m;
            m.payload = "host";
            (void)mailbox.push(std::move(m));
            // Brief sleep so we don't starve the scheduler.
            std::this_thread::sleep_for(std::chrono::milliseconds(10 + (rng() % 20)));
        }
    });

    // Wait for fibers to finish (or deadline).
    io.join();
    host.join();

    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count();

    std::println("  fibers_done={}/{} ops={} yields={} guards={} mb_ops={} elapsed={}s",
                 st.fibers_done.load(), n_fibers, st.ops.load(), st.yields.load(), st.guards.load(),
                 st.mb_ops.load(), elapsed_s);

    // End-of-soak snapshot.
    const HardFailSnapshot after = read_snapshot(cs);
    print_snapshot("after ", after);

    // ── AC2 / AC3: hard-fail assertions ─────────────────────
    // Under production_defaults_active: any non-zero counter fails.
    // Under Soft / Off: observe-only — log and pass.
    auto fail_if_prod = [&](const char* name, bool is_fail) {
        if (production && is_fail) {
            CHECK(false, std::string("3555 AC2: ") + name + " (production mode)");
        } else {
            CHECK(true, std::string("3555 AC3 (Soft): ") + name + " (observed)");
        }
    };

    // (1) steal_safety residual → 1 means ready (zero residual).
    fail_if_prod("steal_safety residual zero",
                 after.residual_zero == 0 || before.residual_zero == 0);
    // Production window: residual MUST be zero end-of-soak.
    CHECK(!production || after.residual_zero != 0,
          "3555 AC2: steal_safety residual zero under production");

    // (2) safepoint_blocked_by_long_mutation p99 ≤ kMailboxP99SLO_us
    //     (max-proxy; any single event > SLO fails).
    fail_if_prod("safepoint_blocked_by_long_mutation_max_us SLO",
                 after.safepoint_max_us > kMailboxP99SLO_us);
    CHECK(after.safepoint_max_us <= kMailboxP99SLO_us || !production,
          "3555 AC2: safepoint_blocked_by_long_mutation_max_us <= kMailboxP99SLO_us");

    // (3) atomic_batch_tenant_isolation_denials_total delta == 0
    const auto tenant_delta = after.tenant_isolation_denials - before.tenant_isolation_denials;
    fail_if_prod("tenant_isolation_denials delta", tenant_delta != 0);
    CHECK(tenant_delta == 0 || !production, "3555 AC2: tenant_isolation_denials_total delta == 0");

    // (4) g_lock_order_violation_total delta == 0
    const auto lock_order_delta = after.lock_order_violations - before.lock_order_violations;
    fail_if_prod("lock_order_violations delta", lock_order_delta != 0);
    CHECK(lock_order_delta == 0 || !production,
          "3555 AC2: g_lock_order_violation_total delta == 0");

    // (5) eventfd_wake_force_safepoint_total delta == 0
    const auto eventfd_delta = after.eventfd_wake_force - before.eventfd_wake_force;
    fail_if_prod("eventfd_wake_force delta", eventfd_delta != 0);
    CHECK(eventfd_delta == 0 || !production,
          "3555 AC2: eventfd_wake_force_safepoint_total delta == 0");

    // (6) fiber_id_mismatch_total delta == 0
    const auto fiber_id_delta = after.fiber_id_mismatch - before.fiber_id_mismatch;
    fail_if_prod("fiber_id_mismatch delta", fiber_id_delta != 0);
    CHECK(fiber_id_delta == 0 || !production, "3555 AC2: fiber_id_mismatch_total delta == 0");

    // Issue #3590: QueryEpoch strict joins the 6 hard-fail path (no 7th
    // counter). Armed production process must report strict; Soft/Off
    // observe-only via fail_if_prod.
    fail_if_prod("query_epoch_strict armed", !aura::core::query_epoch_strict());
    CHECK(aura::core::query_epoch_strict() || !production,
          "3590: epoch strict must be armed under production defaults (#3075)");

    // AC4: env knobs documented + defaults sane.
    CHECK(duration_s >= 1, "3555 AC4: AURA_CHAOS_SOAK_DEPLOY_GATE_DURATION_S sane");
    CHECK(workers >= 8, "3555 AC4: AURA_CHAOS_SOAK_DEPLOY_GATE_WORKERS >= 8");
    CHECK(n_fibers >= 8, "3555 AC4: AURA_CHAOS_SOAK_DEPLOY_GATE_FIBERS >= 8");

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_chaos_soak_production_gate();
}
#endif

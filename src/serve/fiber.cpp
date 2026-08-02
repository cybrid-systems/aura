// serve/fiber.cpp — Stackful fiber implementation
//
// Hot-Update MVP scope (Issue #1943): fiber steal + bridge_epoch
// (g_bridge_epoch_ on Worker) is **out of scope** for the MVP —
// hot-update during stolen-fiber / complex-agent-orchestration paths
// are deferred. See docs/hot-update.md and #1929 / #1931 / #1947 /
// #1950 / #1953 / #1954 for the deferred correctness work.
#include "fiber.h"
#include "scheduler.h"
#include "metrics.h"                      // Issue #2119: adaptive_steal_stats yield/hold
#include "../compiler/messaging_bridge.h" // Issue #285: g_flush_mutation_boundary
#include "../compiler/shape.h"            // Issue #570: record_shape_fiber_refresh
#include "aura_platform.h"
#include "core/gc_hooks.h" // Issue #1364

#include <sys/mman.h>
#include <cassert> // Issue #354 / #2200: assert for yield-during-boundary check
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

import std;
#if AURA_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

namespace aura::serve {

// Issue #810: process-wide Fiber/Scheduler init path counters.
// Exceptions still used for true resource failures (mmap/eventfd);
// successful construction records AuraResult-style ok path.
static std::atomic<std::uint64_t> g_fiber_init_aura_result_ok{0};
static std::atomic<std::uint64_t> g_fiber_init_aura_result_err{0};
static std::atomic<std::uint64_t> g_scheduler_init_aura_result_ok{0};
static std::atomic<std::uint64_t> g_scheduler_init_aura_result_err{0};

extern "C" void aura_evaluator_resume_fiber_migration();
extern "C" void aura_evaluator_post_resume_refresh(); // Issue #1490
// Issue #1595: host-side post-join linear + StableNodeRef enforcement.
extern "C" void aura_evaluator_on_fiber_join(void* joined_fiber);

std::atomic<uint64_t> Fiber::next_id_{1};
std::atomic<std::uint64_t> Fiber::static_gc_pause_attributed_to_mutation_count_{0};
std::atomic<std::uint64_t> Fiber::join_total_{0};
std::atomic<std::uint64_t> Fiber::join_timeout_total_{0};
std::atomic<std::uint64_t> Fiber::join_cancel_total_{0};
std::atomic<std::uint64_t> Fiber::join_wait_us_total_{0};
std::atomic<std::uint64_t> Fiber::join_wait_us_max_{0};
// Issue #2467: counter for JoinStatus::Reclaimed returns
// (target was force-reclaimed via Scheduler::reap_orphans_now
// but body is still executing). Joiner returned Reclaimed
// without calling aura_evaluator_on_fiber_join cleanup hook
// to avoid UAF on shared resources the body may still touch.
std::atomic<std::uint64_t> Fiber::join_reclaim_total_{0};
// Issue #2397: reclaimed-but-body-not-returned gauge + retired counter.
// Bumped in mark_reclaimed / note_body_exit_if_reclaimed / ~Fiber.
// Mirrored into OrchModuleStats via weak C hooks when orch is linked.
std::atomic<std::uint64_t> Fiber::join_drain_residual_still_running_{0};
std::atomic<std::uint64_t> Fiber::join_drain_residual_body_retired_total_{0};
// Issue #2533: residual force-safepoint / CPU budget metrics.
std::atomic<std::uint64_t> Fiber::residual_force_safepoint_total_{0};
std::atomic<std::uint64_t> Fiber::residual_cpu_budget_exceeded_total_{0};
// Issue #1595 process-wide join-path linear enforcement attempts (even without Evaluator).
std::atomic<std::uint64_t> Fiber::join_linear_enforcement_total_{0};
// Issue #2491: process-wide TenantScope install mismatch counter
// (resume detects current capability_tenant_id_ != assigned_tenant_id_).
std::atomic<std::uint64_t> Fiber::static_tenant_scope_mismatch_total_{0};
// Issue #2498: process-wide orphan-root drops on Reclaimed (summed
// across all fibers). Accessor Fiber::orphan_roots_dropped_on_reclaim_total().
// HWM tracks the peak count of pending orphan roots across all fibers
// (snapshot when register adds; cleared when release drops). Surface
// for AC1 root count bound + dashboards.
std::atomic<std::uint64_t> Fiber::orphan_roots_dropped_on_reclaim_total_{0};
std::atomic<std::uint64_t> Fiber::orphan_roots_hwm_{0};

// Issue #2397: optional orch dashboard mirror (weak no-op when orch not linked;
// strong defs in evaluator_fiber_mutation.cpp bump OrchModuleStats).
extern "C" void aura_orch_note_join_drain_reclaim_still_running() __attribute__((weak));
extern "C" void aura_orch_note_join_drain_reclaim_body_retired() __attribute__((weak));
extern "C" void aura_orch_note_join_drain_reclaim_still_running_drop() __attribute__((weak));

namespace {
    // Saturating decrement for still-running gauge (never wraps under 0).
    void still_running_dec_one(std::atomic<std::uint64_t>& g) noexcept {
        auto cur = g.load(std::memory_order_relaxed);
        while (cur > 0 && !g.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        }
    }
} // namespace
// Issue #1597 join latency histogram.
std::atomic<std::uint64_t> Fiber::join_latency_hist_[Fiber::kJoinLatencyHistBuckets]{};

// Issue #618: GC safepoint frequency tuning atomic. Initialized to
// 50 (matches historical every-Nth-allocation heuristic). The
// (orchestration:tune-gc-frequency ratio) primitive writes here;
// the scheduler can opt-in to consult it (follow-up).
namespace {
    std::atomic<std::uint32_t> g_gc_frequency_tune_ratio_{50};
} // namespace
std::atomic<std::uint32_t>& gc_frequency_tune_ratio() noexcept {
    return g_gc_frequency_tune_ratio_;
}

// Issue #1493: C ABI for CompilerMetrics / evaluator hold-time adaptive
// (avoids module GMF forward-declare of C++ namespace symbols).
extern "C" std::uint32_t aura_gc_frequency_tune_ratio_load(void) {
    return g_gc_frequency_tune_ratio_.load(std::memory_order_relaxed);
}
extern "C" void aura_gc_frequency_tune_ratio_store(std::uint32_t v) {
    g_gc_frequency_tune_ratio_.store(v, std::memory_order_relaxed);
}

// TLS: current running fiber (nullptr = worker loop context)
thread_local Fiber* g_current_fiber = nullptr;
// TLS: current worker's dispatch loop context
thread_local WorkerContext* g_worker_ctx = nullptr;

// Issue #213 Cycle 3: function pointers that the Evaluator
// registers at startup. See fiber.h for the rationale.
void* (*g_fiber_setter_)(void*) = nullptr;
void (*g_fiber_sync_mutation_stack_)(void*) = nullptr;
void (*g_fiber_storage_deleter_)(void*) = nullptr;
void (*g_fiber_yield_checkpoint_)(uint8_t) = nullptr;

// Issue #439: C-linkage forward declarations for the
// GC safepoint coordination hooks (defined in
// evaluator_fiber_mutation.cpp). The P0 check_gc_safepoint
// calls aura_evaluator_request_gc_safepoint() to bump
// the requests counter and check whether a guard is
// held (in which case the request is deferred).
extern "C" int aura_evaluator_request_gc_safepoint();
extern "C" void aura_evaluator_wait_for_safepoint(std::uint64_t timeout_ms);
void (*g_fiber_resume_validate_)() = nullptr;
void (*g_fiber_yield_checkpoint_deleter_)(void*) = nullptr;

// Issue #195: per-fiber exception state requires a way to
// query the current fiber's id from the runtime (the JIT
// personality function and aura_exception_* use it). We
// install a hook here that returns the current fiber's id
// (or 0 if no fiber is active). The hook is set up once
// at static-init time.
extern "C" std::uint64_t aura_fiber_current_id() {
    return g_current_fiber ? g_current_fiber->id() : 0;
}

// Issue #451: C-linkage shim for the static aggregate
// counter bumped in check_gc_safepoint(). The
// (query:orchestration-metrics) primitive reads this
// from evaluator_primitives_query.cpp.
extern "C" std::uint64_t aura_fiber_static_gc_pause_attributed_to_mutation() {
    return Fiber::static_gc_pause_attributed_to_mutation_total();
}

// Issue #783: C-linkage shims for the refined work-steal
// metrics (outermost vs inner MutationBoundary split +
// cross-fiber safe steal). Read by the
// (query:orchestration-steal-outermost-stats) primitive.
extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total() {
    return Fiber::static_steal_outermost_mutation_boundary_total();
}
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total() {
    return Fiber::static_steal_inner_mutation_boundary_deferred_total();
}

// Issue #2184: process-wide MutationSafetySnapshot mismatch total.
extern "C" std::uint64_t aura_fiber_static_mutation_steal_snapshot_mismatch_total() {
    return Fiber::mutation_steal_snapshot_mismatch_total();
}
// Issue #2310: process-wide force-deopt total (fail-closed
// enforcement). Bumped by aura_force_deopt_on_steal_snapshot_mismatch
// in worker.cpp try_steal_from success path + refresh_after_fiber_migration
// resume-path re-sample fence (AC2 defense-in-depth).
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_mismatch_force_deopt_total() {
    return Fiber::steal_snapshot_mismatch_force_deopt_total();
}
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total() {
    return Fiber::static_cross_fiber_mutation_safe_steal_total();
}

// Issue #783: static aggregate atomic definitions.
// Mirrors Fiber::static_gc_pause_attributed_to_mutation_count_.
// Default-initialized to 0; bumped from the per-Fiber
// bump helpers (which bump both per-Fiber and static).
std::atomic<std::uint64_t> Fiber::static_steal_outermost_mutation_boundary_count_{0};
std::atomic<std::uint64_t> Fiber::static_steal_inner_mutation_boundary_deferred_count_{0};
std::atomic<std::uint64_t> Fiber::static_cross_fiber_mutation_safe_steal_count_{0};
std::atomic<std::uint64_t> Fiber::static_yield_mutation_boundary_total_{0};
// Issue #2184: MutationSafetySnapshot mismatch under steal/resume.
std::atomic<std::uint64_t> Fiber::mutation_steal_snapshot_mismatch_total_{0};
// Issue #2310: process-wide force-deopt counter (fail-closed
// enforcement). Distinct from mutation_steal_snapshot_mismatch_total_
// which is observed-only.
std::atomic<std::uint64_t> Fiber::steal_snapshot_mismatch_force_deopt_total_{0};
// Issue #2346: resume hard-fail (mark-failed) total.
std::atomic<std::uint64_t> Fiber::steal_snapshot_hard_fail_total_{0};
// Issue #2518: resume safety ticket mismatch total.
std::atomic<std::uint64_t> Fiber::steal_safety_ticket_mismatch_total_{0};

// Issue #2346: C ABI for hard-fail total (query / tests without Fiber type).
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total() {
    return Fiber::steal_snapshot_hard_fail_total();
}
// Issue #2518: C ABI for safety ticket mismatch total.
extern "C" std::uint64_t aura_fiber_static_steal_safety_ticket_mismatch_total() {
    return Fiber::steal_safety_ticket_mismatch_total();
}

// Issue #2346 production canary: strong def in typed_mutation_audit_hooks.cpp
// overrides this weak no-op when the audit TU is linked.
extern "C" __attribute__((weak)) int aura_production_defaults_active_probe() noexcept {
    return 0;
}

// Issue #2372: production Soft lock + test override.
// production_locked: set by apply_production_security_defaults when
// sandbox != off — Soft env is ignored under the lock.
// test_override: 0 = consult env/lock, 1 = force Soft, 2 = force non-Soft.
// Test override wins (mirror set_gc_defer_overflow_policy_for_test).
namespace {
    std::atomic<std::uint8_t> g_steal_snapshot_soft_production_locked{0};
    std::atomic<std::uint8_t> g_steal_snapshot_soft_test_override{0};
} // namespace

void set_steal_snapshot_soft_production_locked(bool v) noexcept {
    g_steal_snapshot_soft_production_locked.store(v ? 1 : 0, std::memory_order_release);
}

bool steal_snapshot_soft_production_locked() noexcept {
    return g_steal_snapshot_soft_production_locked.load(std::memory_order_acquire) != 0;
}

void set_steal_snapshot_soft_for_test(bool soft) noexcept {
    g_steal_snapshot_soft_test_override.store(soft ? 1 : 2, std::memory_order_release);
}

void reset_steal_snapshot_soft_for_test() noexcept {
    g_steal_snapshot_soft_test_override.store(0, std::memory_order_release);
}

bool is_steal_snapshot_soft_mode() noexcept {
    // Issue #2372 AC3: test override wins over production lock + env.
    const auto ov = g_steal_snapshot_soft_test_override.load(std::memory_order_acquire);
    if (ov == 1)
        return true;
    if (ov == 2)
        return false;
    // Issue #2372 AC1: under production lock Soft env is ignored.
    if (g_steal_snapshot_soft_production_locked.load(std::memory_order_acquire) != 0)
        return false;
    const char* v = std::getenv("AURA_STEAL_SNAPSHOT_SOFT");
    return v && v[0] == '1';
}

bool is_steal_snapshot_hard_mode() noexcept {
    if (is_steal_snapshot_soft_mode())
        return false;
    const char* hard = std::getenv("AURA_STEAL_SNAPSHOT_HARD");
    if (hard && hard[0] == '1')
        return true;
    // Production canary (strong probe when audit hooks linked).
    if (aura_production_defaults_active_probe() != 0)
        return true;
    return false;
}

bool is_steal_snapshot_hard_abort() noexcept {
    if (!is_steal_snapshot_hard_mode())
        return false;
    const char* v = std::getenv("AURA_STEAL_SNAPSHOT_HARD_ABORT");
    return v && v[0] == '1';
}

// Issue #2346 / #2518: post-sync resume invariant (Soft metric / Hard mark-failed).
// See decision table on is_steal_snapshot_hard_mode() in fiber.h.
// Issue #2518: if steal stamped resume_safety_ticket_, current even seq must
// match — Guard enter/exit between sample and resume advances safety_seq_ and
// is treated as inconsistent (closes the sample→resume window).
bool Fiber::check_and_enforce_resume_snapshot_invariant() noexcept {
    // Single snapshot sample (happy path: one seqlock read; ticket compare
    // is a plain load of the one-shot flag + field — AC4).
    const auto snap = mutation_safety_snapshot();
    bool ticket_miss = false;
    if (has_resume_safety_ticket_) {
        // Compare against ticket captured in this snap (same even seq as
        // current_safety_ticket on happy path). Mismatch ⇒ mid-window publish.
        if (snap.ticket != resume_safety_ticket_)
            ticket_miss = true;
        // One-shot: consume so non-steal resumes and re-tries are clean.
        clear_resume_safety_ticket();
    }
    if (!mutation_safety_snapshot_inconsistent(snap) && !ticket_miss)
        return true; // consistent — continue
    // Soft always bumps the observed mismatch counter first.
    bump_mutation_steal_snapshot_mismatch();
    if (ticket_miss)
        bump_steal_safety_ticket_mismatch();
    if (!is_steal_snapshot_hard_mode())
        return true; // Soft: continue resume
    // Hard: mark-failed so orch can drain (prefer over silent continue).
    bump_steal_snapshot_hard_fail();
    request_cancel();
    set_state(FiberState::Done);
    if (is_steal_snapshot_hard_abort()) {
        std::fprintf(stderr,
                     "FATAL: Fiber::resume MutationSafetySnapshot inconsistent "
                     "(AURA_STEAL_SNAPSHOT_HARD_ABORT=1, fiber=%llu depth=%zu yield=%u "
                     "ticket_miss=%d)\n",
                     static_cast<unsigned long long>(id_), snap.depth,
                     static_cast<unsigned>(snap.last_yield), ticket_miss ? 1 : 0);
        std::abort();
    }
    return false; // caller must not swapcontext
}
// The runtime-side hook installer (defined in
// aura_jit_runtime.cpp).
extern "C" void aura_set_current_fiber_id_fn(std::uint64_t (*)());
// One-time hook installer via a static initializer.
static int s_fiber_hook_init = (aura_set_current_fiber_id_fn(&aura_fiber_current_id), 0);

Scheduler* g_scheduler = nullptr;

// ── GC safepoint check ────────────────────────────────

void Fiber::check_gc_safepoint() {
    // Issue #2533: residual hard-reclaim poll at cooperative edges.
    // mark_reclaimed already set cancel + force_safepoint. Do NOT call
    // yield() from here (yield → check_gc_safepoint would recurse). Yield
    // path and GC wait continue; reclaimed bodies are not re-dispatched
    // by the scheduler. Pure C++ tight loops without edges remain
    // quarantine-visible via join_drain_residual_still_running.
    if (auto* cur = g_current_fiber) {
        if (cur->is_force_safepoint_requested()) {
            cur->force_safepoint_requested_.store(false, std::memory_order_release);
            // Under MutationBoundary hold, residual body cannot yield yet —
            // count budget exceeded (join still Reclaimed).
            if (aura_evaluator_mutation_boundary_held() != 0 ||
                aura_evaluator_mutation_boundary_depth() > 0) {
                residual_cpu_budget_exceeded_total_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    auto* wctx = g_worker_ctx;
    if (!wctx)
        return;
    auto* gc = wctx->gc_state;
    if (!gc)
        return;
    auto phase = gc->phase.load(std::memory_order_acquire);
    // Issue #439: bump the requests counter + check
    // whether the current thread holds an outermost
    // MutationBoundary guard. The C-linkage shim
    // returns 1 if the request is deferred (caller
    // should yield + retry). The P0 records the
    // request + the deferral; the follow-up wires
    // the actual yield+retry into the wait path.
    const bool holding_mutation = aura_evaluator_mutation_boundary_depth() > 0;
    if (phase == GCPhase::Requested) {
        (void)aura_evaluator_request_gc_safepoint();
        // Issue #451 + #1256: attribute the safepoint wait to
        // a MutationBoundary if one is currently held
        // by the active thread.
        if (holding_mutation) {
            static_gc_pause_attributed_to_mutation_count_.fetch_add(1, std::memory_order_relaxed);
            gc->safepoint_wait_while_mutation_held.fetch_add(1, std::memory_order_relaxed);
            // Issue #1364: process-wide + optional CompilerMetrics mirror
            aura::gc_hooks::note_safepoint_yield_on_mutation();
        }
    }
    if (phase == GCPhase::Requested) {
        // Arrive at safepoint: increment counter
        gc->fibers_at_safepoint.fetch_add(1, std::memory_order_release);
        // Issue #1256: high-res timer around eventfd / spin wait
        // so production can see GC tail latency under mutation hold.
        const auto t0 = std::chrono::steady_clock::now();
        gc->wait_for_resume();
        const auto dt = std::chrono::steady_clock::now() - t0;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(dt).count();
        const auto uus = static_cast<std::uint64_t>(us > 0 ? us : 0);
        gc->eventfd_wakeup_latency_us.fetch_add(static_cast<std::int64_t>(uus),
                                                std::memory_order_relaxed);
        if (holding_mutation) {
            // Issue #1493: export wait duration while mutation-held
            // (process-wide + per-worker long-block signal).
            aura::gc_hooks::note_safepoint_wait_while_mutation(uus);
            if (uus > 1'000) {
                // >1ms wait while holding mutation → long-mutation GC block signal.
                gc->safepoint_blocked_by_long_mutation.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ── Constructor ───────────────────────────────────────

Fiber::Fiber(Func func, size_t stack_size)
    : id_(next_id_++)
    , stack_size_(stack_size)
    , func_(std::move(func)) {

    // 1. Allocate stack via mmap with guard page
    // Guard page is the first page (PROT_NONE)
    size_t guard_size = 4096;
    size_t alloc_size = guard_size + stack_size_;

    void* base =
        ::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        g_fiber_init_aura_result_err.fetch_add(1, std::memory_order_relaxed);
        throw std::system_error(errno, std::generic_category(), "fiber mmap stack");
    }

    // Guard page at the bottom (to catch stack underflow from overflow)
    ::mprotect(base, guard_size, PROT_NONE);
    stack_ = static_cast<char*>(base) + guard_size; // usable starts after guard

    // 2. Create eventfd
#if AURA_HAVE_EVENTFD
    eventfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (eventfd_ == -1) {
        ::munmap(base, alloc_size);
        g_fiber_init_aura_result_err.fetch_add(1, std::memory_order_relaxed);
        throw std::system_error(errno, std::generic_category(), "fiber eventfd");
    }
#else
    // macOS: eventfd unavailable; serve-async disabled. fiber can still
    // be constructed (evaluator registers hooks), but eventfd() == -1
    // means no wakeup mechanism — spawn will never be called in core mode.
    eventfd_ = -1;
#endif

    // 3. Initialize ucontext
    if (::getcontext(&ctx_) == -1) {
        ::munmap(base, alloc_size);
        if (eventfd_ >= 0)
            ::close(eventfd_);
        g_fiber_init_aura_result_err.fetch_add(1, std::memory_order_relaxed);
        throw std::system_error(errno, std::generic_category(), "fiber getcontext");
    }

    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stack_size_;
    ctx_.uc_link = nullptr;

    // makecontext needs function pointer with (int, int) signature on all POSIX
    uint32_t id_high = static_cast<uint32_t>(id_ >> 32);
    uint32_t id_low = static_cast<uint32_t>(id_ & 0xFFFFFFFF);
    ::makecontext(&ctx_, reinterpret_cast<void (*)()>(&trampoline), 2, id_high, id_low);
    g_fiber_init_aura_result_ok.fetch_add(1, std::memory_order_relaxed);
}

// Issue #810 C-linkage readers for observability queries.
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total() {
    return g_fiber_init_aura_result_ok.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total() {
    return g_fiber_init_aura_result_err.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total() {
    return g_scheduler_init_aura_result_ok.load(std::memory_order_relaxed);
}
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total() {
    return g_scheduler_init_aura_result_err.load(std::memory_order_relaxed);
}
extern "C" void aura_fiber_init_record_err() {
    g_fiber_init_aura_result_err.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void aura_scheduler_init_record_ok() {
    g_scheduler_init_aura_result_ok.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void aura_scheduler_init_record_err() {
    g_scheduler_init_aura_result_err.fetch_add(1, std::memory_order_relaxed);
}

// ── Issue #2397: reclaimed vs body-still-running ─────

void Fiber::mark_reclaimed() noexcept {
    // Idempotent: only the first transition reclaimed_ false→true
    // may bump still-running (avoids double-count on repeated reap).
    const bool was = reclaimed_.exchange(true, std::memory_order_acq_rel);
    if (was)
        return;
    // Issue #2533: nudge residual body to cooperative edges (cancel +
    // force-safepoint). Ok/done path above returns early — zero cost.
    request_cancel();
    request_force_safepoint();
    residual_force_safepoint_total_.fetch_add(1, std::memory_order_relaxed);
    // Body already returned — logical reclaim of a finished fiber
    // is a no-op for the still-running gauge (zero extra cost beyond
    // the exchange already paid by the reclaim path).
    if (state_.load(std::memory_order_acquire) == FiberState::Done)
        return;
    still_running_after_reclaim_counted_.store(true, std::memory_order_release);
    join_drain_residual_still_running_.fetch_add(1, std::memory_order_relaxed);
    if (aura_orch_note_join_drain_reclaim_still_running)
        aura_orch_note_join_drain_reclaim_still_running();
}

std::uint64_t Fiber::residual_force_safepoint_total() noexcept {
    return residual_force_safepoint_total_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::residual_cpu_budget_exceeded_total() noexcept {
    return residual_cpu_budget_exceeded_total_.load(std::memory_order_relaxed);
}

void Fiber::note_body_exit_if_reclaimed() noexcept {
    if (!reclaimed_.load(std::memory_order_acquire))
        return;
    // Pair the still-running +1 from mark_reclaimed (exactly once).
    const bool counted =
        still_running_after_reclaim_counted_.exchange(false, std::memory_order_acq_rel);
    if (!counted)
        return;
    still_running_dec_one(join_drain_residual_still_running_);
    join_drain_residual_body_retired_total_.fetch_add(1, std::memory_order_relaxed);
    if (aura_orch_note_join_drain_reclaim_body_retired)
        aura_orch_note_join_drain_reclaim_body_retired();
}

std::uint64_t Fiber::join_drain_residual_still_running() noexcept {
    return join_drain_residual_still_running_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_drain_residual_body_retired_total() noexcept {
    return join_drain_residual_body_retired_total_.load(std::memory_order_relaxed);
}

// ── Destructor ───────────────────────────────────────

Fiber::~Fiber() {
    // Issue #2397: reaper destroyed Fiber while body never returned
    // (or body never started). Drop still-running gauge without
    // bumping retired — body did not exit cleanly after reclaim.
    if (still_running_after_reclaim_counted_.exchange(false, std::memory_order_acq_rel)) {
        still_running_dec_one(join_drain_residual_still_running_);
        if (aura_orch_note_join_drain_reclaim_still_running_drop)
            aura_orch_note_join_drain_reclaim_still_running_drop();
    }
    // Issue #2498: safety-net release. If the Fiber reaches dtor
    // without ever being joined (test fixture dropped the pointer,
    // scheduler owning_fibers_.clear path, etc.), any pending orphan
    // root callbacks still need to fire — otherwise the global table
    // entries they wrap (EnvFrame refs, mailbox refs) leak until the
    // Evaluator / mailbox itself is destroyed. Idempotent: if the
    // join paths already invoked release_orphan_roots(), the table is
    // empty and this is a no-op. Same fail-safe shape as the Reclaimed
    // path inside Fiber::join.
    release_orphan_roots();
    if (eventfd_ >= 0)
        ::close(eventfd_);
    if (stack_) {
        // stack_ = usable start; the mmap base is one guard page before
        auto* base = static_cast<char*>(stack_) - 4096;
        ::munmap(base, 4096 + stack_size_);
    }
    // Issue #213 Cycle 3: free the per-fiber mutation stack
    // storage. The pointer was lazily allocated by
    // Evaluator::active_mutation_stack() on first use. We
    // only know it as void* here (fiber.h doesn't have the
    // MutationCheckpoint type), so the Evaluator's accessor
    // casts it back. The destructor just frees the void*
    // — the Evaluator accessor is the one that knows the
    // actual vector type.
    if (void* p = mutation_stack_storage_.load(std::memory_order_acquire)) {
        // The Evaluator accessor lazy-allocates; it owns the
        // pointer. But for cleanup, we cast to the right type
        // and delete. This requires the Evaluator's type to
        // be visible. Use a function pointer that the
        // Evaluator registers at startup to do the cleanup
        // (avoids a circular include).
        if (g_fiber_storage_deleter_) {
            g_fiber_storage_deleter_(p);
        }
        mutation_stack_storage_.store(nullptr, std::memory_order_release);
    }
    if (void* p = yield_checkpoint_storage_.load(std::memory_order_acquire)) {
        if (g_fiber_yield_checkpoint_deleter_) {
            g_fiber_yield_checkpoint_deleter_(p);
        }
        yield_checkpoint_storage_.store(nullptr, std::memory_order_release);
    }
}

// ── Resume — worker → fiber ───────────────────────────
// Called from a WorkerThread's dispatch loop.
// Saves the worker's loop context into g_worker_ctx->uctx,
// then swaps to the fiber's context.
// When the fiber yields (or finishes), control returns here.

void Fiber::resume() {
    auto* wctx = g_worker_ctx;
    if (!wctx) {
        std::fprintf(stderr, "fiber[%lu]: resume called with no worker context\n",
                     (unsigned long)id_);
        return;
    }
    // Issue #2468: pre-check state to prevent UB on Done/Reclaimed
    // fibers. ~Fiber() unmaps the stack (stack_ via munmap) when the
    // fiber is destroyed (e.g., owned_fibers_.clear() in ~Scheduler()).
    // If a caller invokes resume() on a fiber whose state_==Done
    // (body already finished) or reclaimed_==true (hard-reclaimed by
    // Scheduler::reap_orphans_now), swapcontext would be called on
    // unmapped memory → undefined behavior (typically SIGSEGV).
    // Early-return with a stderr message so the misuse is visible in
    // logs but doesn't crash the process. The WorkerThread dispatch
    // loop and Fiber::join host-thread path are the documented callers.
    if (state_.load(std::memory_order_acquire) == FiberState::Done) {
        std::fprintf(stderr, "fiber[%lu]: resume on Done fiber (no-op, UB guard #2468)\n",
                     (unsigned long)id_);
        return;
    }
    if (reclaimed_.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "fiber[%lu]: resume on reclaimed fiber (no-op, UB guard #2468)\n",
                     (unsigned long)id_);
        return;
    }

    // Issue #2119: close MutationBoundary yield hold-time sample.
    if (last_yield_reason() == YieldReason::MutationBoundary) {
        const auto enter = mutation_boundary_yield_enter_ns();
        if (enter != 0) {
            using namespace std::chrono;
            const auto now = static_cast<std::uint64_t>(
                duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
            if (now >= enter) {
                metrics::adaptive_steal_stats().yield_mutation_boundary_hold_ns_total.fetch_add(
                    now - enter, std::memory_order_relaxed);
            }
            clear_mutation_boundary_yield_enter_ns();
        }
    }

    auto prev = g_current_fiber;
    g_current_fiber = this;
    // Issue #213 Cycle 3: also update the Evaluator's
    // thread_local current_fiber pointer so the
    // active_mutation_stack() accessor can find the
    // per-fiber stack. We use a function pointer that the
    // Evaluator registers at startup (avoids the circular
    // include between fiber.h and evaluator.ixx).
    auto prev_fiber_void = g_fiber_setter_ ? g_fiber_setter_(this) : nullptr;
    // Issue #588: bind per-fiber mutation stack on worker resume.
    // Issue #1992: storage is std::atomic<void*>; load
    // the pointer before passing to the sync callback
    // (plain field read would be UB on a non-atomic
    // read of an atomic field).
    if (g_fiber_sync_mutation_stack_)
        g_fiber_sync_mutation_stack_(mutation_stack_storage_.load(std::memory_order_acquire));
    // Issue #2491: install TenantScope from assigned_tenant_id (when set
    // + production sandbox active). Bridge is a C-linkage shim that the
    // Evaluator module overrides; weak no-op when not linked.
    aura_fiber_install_tenant_scope_for_resume(this);
    // Issue #2184 / #2346: post-sync snapshot invariant.
    // Soft: bump mismatch metric, continue. Hard: mark-failed (Done+cancel),
    // skip swapcontext so inconsistent code never runs (orch can drain).
    if (!check_and_enforce_resume_snapshot_invariant()) {
        // Hard-fail: restore TLS and return without parking the body.
        if (g_fiber_setter_)
            g_fiber_setter_(prev_fiber_void);
        g_current_fiber = prev;
        return;
    }
    // Issue #485: transfer mutation stack + bump migration stats.
    aura_evaluator_resume_fiber_migration();
    state_.store(FiberState::Running, std::memory_order_release);

    // Swap from worker's loop context to fiber's context
    if (::swapcontext(&wctx->uctx, &ctx_) == -1) {
        std::fprintf(stderr, "fiber[%lu]: resume swapcontext failed: %s\n", (unsigned long)id_,
                     std::strerror(errno));
    }

    // Issue #264: validate yield-boundary checkpoint after resume.
    if (g_fiber_resume_validate_)
        g_fiber_resume_validate_();
    // Issue #2491: release TenantScope after the fiber yields back to the
    // worker. Restores the previous principal so a subsequent resume of a
    // different fiber on the same worker starts from a clean baseline.
    aura_fiber_release_tenant_scope_after_yield();

    // Issue #453: panic checkpoint transfer on fiber migration.
    // After the resume returns, check whether a pending panic
    // checkpoint exists on the resumed fiber's evaluator. If so,
    // call the transfer trampoline (bumps the metric; re-stamps
    // per-fiber storage as a follow-up). The trampoline is a
    // no-op when the bridge hook is null.
    if (aura::messaging::g_pending_panic_checkpoint &&
        aura::messaging::g_pending_panic_checkpoint() &&
        aura::messaging::g_transfer_panic_checkpoint) {
        aura::messaging::g_transfer_panic_checkpoint();
    }

    // Issue #1490 / #1580 / #1592 / #1608 / #1612 / #1631 / #2194: MANDATE
    // EnvFrame / bridge_epoch refresh + linear re-pin + MacroIntroduced
    // marker/provenance + orphan GC-defer clear after resume validate.
    // #1631/#2194: non-optional on resume main path — steal + GC + concurrent
    // mutate must not leave version_/bridge_epoch / pins / defer dangling.
    // Path: aura_evaluator_post_resume_refresh → refresh_after_fiber_migration
    //   → clear_gc_defer (orphan prev host) + refresh_stale_frames_after_steal
    //   → probe_and_repin_linear_on_steal + restamp_pinned StableNodeRefs
    //   → refresh_stale_macro_frames + clear_resume_refresh_hints.
    aura_evaluator_post_resume_refresh();

    if (g_fiber_setter_)
        g_fiber_setter_(prev_fiber_void);
    g_current_fiber = prev;
}

// ── Yield — fiber → worker ────────────────────────────
// ── Issue #2200: production hard-block yield under MutationBoundary ──
//
// #354 only DEBUG-assert / release-fprintf and still called swapcontext.
// That left a generic yield-inside-Guard hole (mailbox #2188 is separate).
// Production policy: while held (or depth>0), never park / stealable yield.
// Mutate bodies must finish or mark_failed — long work exits boundary first.
namespace {
    std::atomic<std::uint64_t> g_yield_while_mutation_held_total{0};

    // why: 1 = held flag, 2 = depth>0 (defense in depth)
    [[nodiscard]] bool yield_blocked_by_mutation_boundary(std::uint8_t* why_out) noexcept {
        // Prefer messaging bridge (set while Evaluator yield-hook is live).
        if (aura::messaging::g_mutation_boundary_held &&
            aura::messaging::g_mutation_boundary_held()) {
            if (why_out)
                *why_out = 1;
            return true;
        }
        // C-linkage held (covers held_ + depth fallback in evaluator_fiber_mutation).
        if (aura_evaluator_mutation_boundary_held() != 0) {
            if (why_out)
                *why_out = 1;
            return true;
        }
        if (aura_evaluator_mutation_boundary_depth() > 0) {
            if (why_out)
                *why_out = 2;
            return true;
        }
        return false;
    }

    void note_yield_rejected_under_mutation_boundary(std::uint8_t why) noexcept {
        g_yield_while_mutation_held_total.fetch_add(1, std::memory_order_relaxed);
        auto& s = metrics::adaptive_steal_stats();
        s.yield_while_mutation_held_total.fetch_add(1, std::memory_order_relaxed);
        s.last_yield_rejected_reason.store(why, std::memory_order_relaxed);
        // Issue #2324: removed the debug-mode assert(false) here. Test
        // #2200 deliberately triggers Fiber::yield under a live
        // MutationBoundary to verify the rejection contract (counter
        // bumps, no swapcontext). The metric bump above is sufficient
        // signal; production forensics remains opt-in via the env-var-
        // gated hard abort below (AURA_YIELD_HELD_ABORT=1).
        // Optional hard abort for production forensics.
        static const bool abort_on_reject = []() noexcept {
            const char* e = std::getenv("AURA_YIELD_HELD_ABORT");
            return e != nullptr && e[0] != '\0' && e[0] != '0' && e[0] != 'f' && e[0] != 'F';
        }();
        if (abort_on_reject) {
            std::fprintf(stderr,
                         "FATAL: Fiber::yield rejected under MutationBoundary "
                         "(AURA_YIELD_HELD_ABORT=1, why=%u)\n",
                         static_cast<unsigned>(why));
            std::abort();
        }
    }
} // namespace

// Process-wide reject total (tests / C ABI without metrics module).
std::uint64_t Fiber::yield_while_mutation_held_total() noexcept {
    return g_yield_while_mutation_held_total.load(std::memory_order_relaxed);
}

// Static: called from within a fiber's execution.
// Swaps back to g_worker_ctx (the current worker's dispatch loop).
// After this, the fiber is suspended. The worker's loop will
// re-enqueue or wait depending on the fiber's state.

void Fiber::yield() {
    auto* wctx = g_worker_ctx;
    if (!wctx) {
        std::fprintf(stderr, "fiber: yield called with no worker context\n");
        return;
    }

    // Check GC safepoint before yielding (P2)
    check_gc_safepoint();

    auto* fb = g_current_fiber;
    if (!fb)
        return;

    // Issue #354 / #2200: hard-block yield while MutationBoundary is
    // live. Guard holds workspace write lock; park/steal here deadlocks
    // multi-agent orchestration. Production: early-return (no swapcontext).
    // Debug: assert. Optional AURA_YIELD_HELD_ABORT=1 aborts in release.
    {
        std::uint8_t why = 0;
        if (yield_blocked_by_mutation_boundary(&why)) {
            note_yield_rejected_under_mutation_boundary(why);
            return; // no swapcontext — AC1
        }
    }

    // Mark as explicit yield (safe to steal)
    fb->set_yield_reason(YieldReason::Explicit);

    if (g_fiber_yield_checkpoint_)
        g_fiber_yield_checkpoint_(static_cast<uint8_t>(YieldReason::Explicit));

    // Swap from fiber's context back to worker's loop context
    if (::swapcontext(&fb->ctx_, &wctx->uctx) == -1) {
        std::fprintf(stderr, "fiber: yield swapcontext failed: %s\n", std::strerror(errno));
    }
}

// ── yield(YieldReason) — yield with reason ────────────

void Fiber::yield(YieldReason reason) {
    auto* wctx = g_worker_ctx;
    if (!wctx) {
        std::fprintf(stderr, "fiber: yield called with no worker context\n");
        return;
    }

    auto* fb = g_current_fiber;
    if (!fb)
        return;

    // Check GC safepoint before yielding (P2)
    check_gc_safepoint();

    // Issue #354 / #2200: same hard gate for all YieldReason overloads
    // (Explicit / MutationBoundary / OperationBoundary / PassPipeline / …).
    // Must run before per-reason counters so rejected attempts are not
    // counted as successful yields.
    {
        std::uint8_t why = 0;
        if (yield_blocked_by_mutation_boundary(&why)) {
            note_yield_rejected_under_mutation_boundary(why);
            return; // no swapcontext — AC1
        }
    }

    // Record the yield reason for scheduler inspection
    fb->set_yield_reason(reason);

    // Issue #451: bump the per-reason orchestration
    // observability counter. The (query:orchestration-metrics)
    // primitive reads these to compute yield breakdown.
    switch (reason) {
        case YieldReason::BlockingIO:
            fb->bump_yield_blocking_io();
            break;
        case YieldReason::MutationBoundary:
            fb->bump_yield_mutation_boundary();
            // Issue #2119: process-wide total + hold-time start (resume closes).
            {
                using namespace std::chrono;
                const auto ns = static_cast<std::uint64_t>(
                    duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
                fb->note_mutation_boundary_yield_enter_ns(ns);
                metrics::adaptive_steal_stats().yield_mutation_boundary_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            aura::compiler::shape::record_shape_fiber_refresh();
            break;
        case YieldReason::Explicit:
            fb->bump_yield_explicit();
            break;
        case YieldReason::SchedulerSteal:
            fb->bump_yield_scheduler_steal();
            break;
        case YieldReason::OperationBoundary:
            fb->bump_yield_operation_boundary();
            break;
        case YieldReason::PassPipeline:
            // Issue #1085: dedicated counter (was incorrectly bumping Explicit).
            fb->bump_yield_pass_pipeline();
            break;
    }

    // If blocking IO, set state to Waiting (IO thread will wake via epoll)
    if (reason == YieldReason::BlockingIO) {
        fb->set_state(FiberState::Waiting);
    }

    // Issue #285: explicit mutation-boundary flush before swapcontext
    // when yielding from inside a mutation boundary. This makes the
    // version bump + per-fiber stack commit visible to other fibers
    // at the precise yield point, eliminating the last race window.
    // The flush is a no-op when no boundary is active (the trampoline
    // inside evaluator_fiber_mutation.cpp checks yield_hook_evaluator
    // and returns early if nullptr).
    if (reason == YieldReason::MutationBoundary && aura::messaging::g_flush_mutation_boundary) {
        aura::messaging::g_flush_mutation_boundary();
    }

    // Issue #453 / #1489: when yielding from a mutation boundary
    // AND a pending panic checkpoint exists, arm process-wide GC
    // defer (via block_gc trampoline → gc_hooks depth) so
    // GCCollector / compact_sweep skip reclaim until recovery.
    // Cheap: one bridge call + thread-local read; no-op without
    // an active guard checkpoint.
    if (reason == YieldReason::MutationBoundary && aura::messaging::g_pending_panic_checkpoint &&
        aura::messaging::g_pending_panic_checkpoint() &&
        aura::messaging::g_block_gc_for_pending_checkpoint) {
        aura::messaging::g_block_gc_for_pending_checkpoint();
    }

    if (g_fiber_yield_checkpoint_)
        g_fiber_yield_checkpoint_(static_cast<uint8_t>(reason));

    // Swap from fiber's context back to worker's loop context
    if (::swapcontext(&fb->ctx_, &wctx->uctx) == -1) {
        std::fprintf(stderr, "fiber: yield swapcontext failed: %s\n", std::strerror(errno));
    }
}

// ── Trampoline — first entry point when fiber starts ──

void Fiber::trampoline(uint32_t /*high*/, uint32_t /*low*/) {
    if (g_current_fiber) {
        g_current_fiber->set_state(FiberState::Running);
        g_current_fiber->func_();
        // Function returned — fiber is done
        g_current_fiber->set_state(FiberState::Done);
        // Issue #2397: if hard-reclaimed while body was still
        // executing, pair still-running gauge + bump retired.
        g_current_fiber->note_body_exit_if_reclaimed();
    }
    // Yield back to worker's loop context
    Fiber::yield();
}

// ── Issue #1584: structured Fiber::join ─────────────────

std::uint64_t Fiber::join_total() noexcept {
    return join_total_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_timeout_total() noexcept {
    return join_timeout_total_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_cancel_total() noexcept {
    return join_cancel_total_.load(std::memory_order_relaxed);
}
// Issue #2467: accessor for join_reclaim_total_ counter
// (JoinStatus::Reclaimed returns when target force-reclaimed
// but body still executing — joiner must defer cleanup).
std::uint64_t Fiber::join_reclaim_total() noexcept {
    return join_reclaim_total_.load(std::memory_order_relaxed);
}

// Issue #2498: epoch-scoped off-stack orphan-root table. Body code
// (running under Fiber::resume) registers a drop callback for each
// global table entry it adds (EnvFrame ref, mailbox ref, external
// handle). The callback captures the slot pointer + owning Evaluator
// (or just the unregister fn + ctx). On hard-reclaim (JoinStatus::
// Reclaimed) or natural Done, the Fiber invokes the callbacks to
// release the global table entries WITHOUT touching the body's running
// stack. The body keeps its stack copies; only the global table
// entries (which the body isn't directly accessing anymore) are
// released. This breaks the design leak from #2467/#2468/#2469
// (non-yielding body after hard-reclaim → join cleanup deferred
// forever → global table entries accumulate).
//
// Thread safety: registrations come from the body's worker thread
// (the Fiber's executing thread); release calls come from the
// joiner thread (or from ~Fiber on the destroying thread). Both
// sides take orphan_roots_mtx_; release invokes callbacks OUTSIDE
// the lock to keep the critical section short and avoid re-entrant
// lock cycles (drop callbacks may acquire other locks).
void Fiber::register_orphan_root_release(std::function<void()> drop) noexcept {
    if (!drop)
        return;
    std::lock_guard<std::mutex> lk(orphan_roots_mtx_);
    orphan_root_releases_.push_back(std::move(drop));
    // Bump HWM (snapshot of pending count across all fibers when
    // the table grows). Decay happens on release; HWM is monotonic
    // until reset (orchestrator reset hook).
    const auto cur = orphan_root_releases_.size();
    auto prev = orphan_roots_hwm_.load(std::memory_order_relaxed);
    while (static_cast<std::uint64_t>(cur) > prev &&
           !orphan_roots_hwm_.compare_exchange_weak(prev, static_cast<std::uint64_t>(cur),
                                                    std::memory_order_relaxed)) {
    }
}

// Returns the number of callbacks invoked. Idempotent: subsequent
// calls on the same Fiber return 0 (the table is cleared on first
// release). Bumps orphan_roots_dropped_on_reclaim_total_ on every
// successful invocation (for AC1 metrics + dashboards).
std::size_t Fiber::release_orphan_roots() noexcept {
    std::vector<std::function<void()>> drops;
    {
        std::lock_guard<std::mutex> lk(orphan_roots_mtx_);
        if (orphan_root_releases_.empty())
            return 0;
        drops.swap(orphan_root_releases_);
    }
    // Invoke OUTSIDE the lock. A drop callback may acquire other
    // locks (Evaluator mutex, mailbox mutex, GC root table) — holding
    // orphan_roots_mtx_ across the call would risk re-entrant cycles.
    std::size_t n = 0;
    for (auto& d : drops) {
        if (d) {
            d();
            ++n;
        }
    }
    orphan_roots_dropped_on_reclaim_total_.fetch_add(n, std::memory_order_relaxed);
    return n;
}

[[nodiscard]] bool Fiber::has_orphan_roots() const noexcept {
    std::lock_guard<std::mutex> lk(orphan_roots_mtx_);
    return !orphan_root_releases_.empty();
}

std::uint64_t Fiber::orphan_roots_dropped_on_reclaim_total() noexcept {
    return orphan_roots_dropped_on_reclaim_total_.load(std::memory_order_relaxed);
}

std::uint64_t Fiber::orphan_roots_hwm() noexcept {
    return orphan_roots_hwm_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_wait_us_total() noexcept {
    return join_wait_us_total_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_wait_us_max() noexcept {
    return join_wait_us_max_.load(std::memory_order_relaxed);
}

std::uint64_t Fiber::join_linear_enforcement_total() noexcept {
    return join_linear_enforcement_total_.load(std::memory_order_relaxed);
}

std::uint64_t Fiber::join_latency_hist(std::size_t bucket) noexcept {
    if (bucket >= kJoinLatencyHistBuckets)
        return 0;
    return join_latency_hist_[bucket].load(std::memory_order_relaxed);
}

std::uint64_t Fiber::join_latency_hist_sum() noexcept {
    std::uint64_t s = 0;
    for (std::size_t i = 0; i < kJoinLatencyHistBuckets; ++i)
        s += join_latency_hist_[i].load(std::memory_order_relaxed);
    return s;
}

// C ABI for observability primitives (avoid pulling fiber.h into obs partitions).
extern "C" std::uint64_t aura_fiber_join_linear_enforcement_total() {
    return Fiber::join_linear_enforcement_total();
}

JoinResult Fiber::join(Fiber* target, std::optional<std::uint64_t> timeout_ms) {
    join_total_.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](JoinStatus st) -> JoinResult {
        const auto us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        join_wait_us_total_.fetch_add(us, std::memory_order_relaxed);
        auto prev = join_wait_us_max_.load(std::memory_order_relaxed);
        while (us > prev &&
               !join_wait_us_max_.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
        }
        // Issue #1597: coarse join latency histogram buckets (µs).
        {
            std::size_t b = 4;
            if (us < 100)
                b = 0;
            else if (us < 1000)
                b = 1;
            else if (us < 10000)
                b = 2;
            else if (us < 100000)
                b = 3;
            join_latency_hist_[b].fetch_add(1, std::memory_order_relaxed);
        }
        if (st == JoinStatus::Timeout)
            join_timeout_total_.fetch_add(1, std::memory_order_relaxed);
        else if (st == JoinStatus::Cancelled)
            join_cancel_total_.fetch_add(1, std::memory_order_relaxed);
        // Issue #2467: bumped when joiner returns Reclaimed (target
        // force-reclaimed but body still executing). Surfaces in
        // (query:fiber-metrics) + dashboards so operators can see
        // when the UAF-prone path triggers.
        else if (st == JoinStatus::Reclaimed)
            join_reclaim_total_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1595: successful join → process counter + host-side probe/repin.
        // Skip deep Evaluator work when called from a fiber stack (small stacks);
        // process counter still advances so dashboards see join-path liveness.
        if (st == JoinStatus::Ok && target != nullptr) {
            join_linear_enforcement_total_.fetch_add(1, std::memory_order_relaxed);
            if (g_current_fiber == nullptr)
                aura_evaluator_on_fiber_join(static_cast<void*>(target));
        }
        return JoinResult{st, us};
    };

    if (!target || target == g_current_fiber)
        return finish(JoinStatus::Invalid);
    // Issue #2467: reclaimed-but-not-done path. The body fiber is
    // STILL EXECUTING on a worker (non-yielding tight loop after
    // the cooperative drain window expired). Return Reclaimed
    // WITHOUT calling aura_evaluator_on_fiber_join — that hook
    // releases shared resources (mailbox refs, env frames,
    // external handles) which the body may still dereference.
    // Cleanup is deferred until Fiber destructor runs after
    // state_==Done. For non-yielding bodies this may never happen
    // (accept the leak vs the UAF).
    if (target->is_reclaimed() && !target->is_done()) {
        // Issue #2498: drop off-stack orphan roots (EnvFrame/mailbox refs
        // the body registered globally) without touching the body's running
        // stack. Body stack copies remain valid; the global table entries
        // (which the body isn't directly accessing anymore) are released
        // here. Same fail-safe shape as pin_contract_held at #2266.
        target->release_orphan_roots();
        return finish(JoinStatus::Reclaimed);
    }
    if (target->is_done())
        return finish(JoinStatus::Ok);

    const bool has_deadline = timeout_ms.has_value();
    const auto deadline = has_deadline ? t0 + std::chrono::milliseconds(*timeout_ms)
                                       : std::chrono::steady_clock::time_point::max();

    // Fiber-context path: register on scheduler joiner_map and park.
    if (g_current_fiber != nullptr && g_scheduler != nullptr) {
        // Fast re-check under race with completion.
        if (target->is_done())
            return finish(JoinStatus::Ok);
        // Issue #2467: same Reclaimed check under fiber-context path —
        // avoids infinite spin when target is reclaimed but body
        // hasn't yielded (state_ never reaches Done).
        if (target->is_reclaimed() && !target->is_done()) {
            // Issue #2498: drop off-stack orphan roots (see top-level
            // Reclaimed path above for rationale). Idempotent + safe
            // to call from the scheduler pre-check.
            target->release_orphan_roots();
            return finish(JoinStatus::Reclaimed);
        }
        if (!g_scheduler->add_joiner(target->id(), g_current_fiber)) {
            // Target vanished or not registered — recheck Done.
            if (target->is_done())
                return finish(JoinStatus::Ok);
            return finish(JoinStatus::Invalid);
        }

        // Wait loop: BlockingIO yield parks until target Done wakes us
        // (or we poll for timeout/cancel via Explicit yields when deadline).
        while (!target->is_done()) {
            // Issue #2467: bail out on reclaim to avoid infinite spin.
            // body will keep running until it eventually yields/returns,
            // but our join is done — caller handles Reclaimed status.
            if (target->is_reclaimed()) {
                // Issue #2498: drop off-stack orphan roots (see top-level
                // Reclaimed path above for rationale). Idempotent.
                target->release_orphan_roots();
                return finish(JoinStatus::Reclaimed);
            }
            if (g_current_fiber->is_cancel_requested()) {
                g_scheduler->remove_joiner(target->id(), g_current_fiber);
                return finish(JoinStatus::Cancelled);
            }
            if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
                g_scheduler->remove_joiner(target->id(), g_current_fiber);
                return finish(JoinStatus::Timeout);
            }
            if (has_deadline) {
                // Timeout path: short Explicit yields so steal/GC can progress
                // and we can re-check the deadline without busy-spinning the
                // worker forever. Joiner stays registered for eventfd wake.
                Fiber::yield(YieldReason::Explicit);
            } else {
                g_current_fiber->set_state(FiberState::Waiting);
                Fiber::yield(YieldReason::BlockingIO);
                // After resume: drain eventfd (non-blocking).
                int evfd = g_current_fiber->eventfd();
                if (evfd >= 0) {
                    std::uint64_t val = 0;
                    while (::read(evfd, &val, sizeof(val)) > 0) {
                    }
                }
                g_current_fiber->set_state(FiberState::Running);
            }
        }
        g_scheduler->remove_joiner(target->id(), g_current_fiber);
        return finish(JoinStatus::Ok);
    }

    // Host-thread path (tests without active fiber context).
    while (!target->is_done()) {
        // Issue #2467: same Reclaimed check on host-thread path.
        if (target->is_reclaimed()) {
            // Issue #2498: drop off-stack orphan roots (see top-level
            // Reclaimed path above for rationale). Idempotent; host
            // thread path doesn't go through scheduler but the drop
            // is independent of scheduler state.
            target->release_orphan_roots();
            return finish(JoinStatus::Reclaimed);
        }
        if (g_current_fiber && g_current_fiber->is_cancel_requested())
            return finish(JoinStatus::Cancelled);
        if (has_deadline && std::chrono::steady_clock::now() >= deadline)
            return finish(JoinStatus::Timeout);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return finish(JoinStatus::Ok);
}

JoinResult Fiber::join(std::span<Fiber* const> targets, std::optional<std::uint64_t> timeout_ms) {
    if (targets.empty())
        return JoinResult{JoinStatus::Ok, 0};

    const auto t0 = std::chrono::steady_clock::now();
    JoinResult last{JoinStatus::Ok, 0};
    for (Fiber* t : targets) {
        std::optional<std::uint64_t> remaining = timeout_ms;
        if (timeout_ms.has_value()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
            if (elapsed >= static_cast<std::int64_t>(*timeout_ms)) {
                join_timeout_total_.fetch_add(1, std::memory_order_relaxed);
                last.status = JoinStatus::Timeout;
                last.wait_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count());
                return last;
            }
            remaining =
                static_cast<std::uint64_t>(*timeout_ms - static_cast<std::uint64_t>(elapsed));
        }
        last = join(t, remaining);
        if (last.status != JoinStatus::Ok)
            return last;
    }
    // Aggregate wait time for the batch.
    last.wait_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    return last;
}

} // namespace aura::serve

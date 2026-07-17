// serve/fiber.cpp — Stackful fiber implementation
#include "fiber.h"
#include "scheduler.h"
#include "../compiler/messaging_bridge.h" // Issue #285: g_flush_mutation_boundary
#include "../compiler/shape.h"            // Issue #570: record_shape_fiber_refresh
#include "aura_platform.h"
#include "core/gc_hooks.h" // Issue #1364

#include <sys/mman.h>
#include <cassert> // Issue #354: assert for yield-during-boundary check
#include <unistd.h>
#include <atomic>
#include <chrono>
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
// Issue #1595 process-wide join-path linear enforcement attempts (even without Evaluator).
std::atomic<std::uint64_t> Fiber::join_linear_enforcement_total_{0};

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
// The runtime-side hook installer (defined in
// aura_jit_runtime.cpp).
extern "C" void aura_set_current_fiber_id_fn(std::uint64_t (*)());
// One-time hook installer via a static initializer.
static int s_fiber_hook_init = (aura_set_current_fiber_id_fn(&aura_fiber_current_id), 0);

Scheduler* g_scheduler = nullptr;

// ── GC safepoint check ────────────────────────────────

void Fiber::check_gc_safepoint() {
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

// ── Destructor ───────────────────────────────────────

Fiber::~Fiber() {
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
    if (mutation_stack_storage_) {
        // The Evaluator accessor lazy-allocates; it owns the
        // pointer. But for cleanup, we cast to the right type
        // and delete. This requires the Evaluator's type to
        // be visible. Use a function pointer that the
        // Evaluator registers at startup to do the cleanup
        // (avoids a circular include).
        if (g_fiber_storage_deleter_) {
            g_fiber_storage_deleter_(mutation_stack_storage_);
        }
        mutation_stack_storage_ = nullptr;
    }
    if (yield_checkpoint_storage_) {
        if (g_fiber_yield_checkpoint_deleter_) {
            g_fiber_yield_checkpoint_deleter_(yield_checkpoint_storage_);
        }
        yield_checkpoint_storage_ = nullptr;
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
    if (g_fiber_sync_mutation_stack_)
        g_fiber_sync_mutation_stack_(mutation_stack_storage_);
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

    // Issue #1490 / #1580 / #1592: force EnvFrame / bridge_epoch refresh +
    // linear re-pin + StableNodeRef auto-restamp + linear ownership enforce
    // after resume validate (pairs with pre-swap migration refresh in
    // aura_evaluator_resume_fiber_migration → complete_post_resume_steal_refresh).
    aura_evaluator_post_resume_refresh();

    if (g_fiber_setter_)
        g_fiber_setter_(prev_fiber_void);
    g_current_fiber = prev;
}

// ── Yield — fiber → worker ────────────────────────────
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

    // Issue #354: "yield while holding a mutation
    // boundary" check. The MutationBoundaryGuard
    // holds the workspace write lock for its
    // lifetime; yielding inside a mutate:*
    // primitive body would release the fiber's
    // view of the lock state and risk deadlock /
    // starvation. In debug builds we assert; in
    // release builds we log + continue (the
    // production-readiness path doesn't crash).
    // The bridge function is nullptr when no
    // Evaluator is wired (test-binary), in which
    // case we skip the check.
    if (aura::messaging::g_mutation_boundary_held && aura::messaging::g_mutation_boundary_held()) {
#ifndef NDEBUG
        assert(false && "Fiber::yield called while a "
                        "MutationBoundaryGuard is alive");
#else
        std::fprintf(stderr, "WARNING: Fiber::yield called while a "
                             "MutationBoundaryGuard is alive "
                             "(forward-looking Issue #354 check)\n");
#endif
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
std::uint64_t Fiber::join_wait_us_total() noexcept {
    return join_wait_us_total_.load(std::memory_order_relaxed);
}
std::uint64_t Fiber::join_wait_us_max() noexcept {
    return join_wait_us_max_.load(std::memory_order_relaxed);
}

std::uint64_t Fiber::join_linear_enforcement_total() noexcept {
    return join_linear_enforcement_total_.load(std::memory_order_relaxed);
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
        if (st == JoinStatus::Timeout)
            join_timeout_total_.fetch_add(1, std::memory_order_relaxed);
        else if (st == JoinStatus::Cancelled)
            join_cancel_total_.fetch_add(1, std::memory_order_relaxed);
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
        if (!g_scheduler->add_joiner(target->id(), g_current_fiber)) {
            // Target vanished or not registered — recheck Done.
            if (target->is_done())
                return finish(JoinStatus::Ok);
            return finish(JoinStatus::Invalid);
        }

        // Wait loop: BlockingIO yield parks until target Done wakes us
        // (or we poll for timeout/cancel via Explicit yields when deadline).
        while (!target->is_done()) {
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

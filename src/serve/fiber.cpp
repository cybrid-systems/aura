// serve/fiber.cpp — Stackful fiber implementation
#include "fiber.h"
#include "scheduler.h"
#include "../compiler/messaging_bridge.h" // Issue #285: g_flush_mutation_boundary

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <system_error>

namespace aura::serve {

std::atomic<uint64_t> Fiber::next_id_{1};

// TLS: current running fiber (nullptr = worker loop context)
thread_local Fiber* g_current_fiber = nullptr;
// TLS: current worker's dispatch loop context
thread_local WorkerContext* g_worker_ctx = nullptr;

// Issue #213 Cycle 3: function pointers that the Evaluator
// registers at startup. See fiber.h for the rationale.
void* (*g_fiber_setter_)(void*) = nullptr;
void (*g_fiber_storage_deleter_)(void*) = nullptr;
void (*g_fiber_yield_checkpoint_)(uint8_t) = nullptr;
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
    if (phase == GCPhase::Requested) {
        // Arrive at safepoint: increment counter
        gc->fibers_at_safepoint.fetch_add(1, std::memory_order_release);
        // Spin-wait until GC completes
        gc->wait_for_resume();
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
    if (base == MAP_FAILED)
        throw std::system_error(errno, std::generic_category(), "fiber mmap stack");

    // Guard page at the bottom (to catch stack underflow from overflow)
    ::mprotect(base, guard_size, PROT_NONE);
    stack_ = static_cast<char*>(base) + guard_size; // usable starts after guard

    // 2. Create eventfd
    eventfd_ = ::eventfd(0, EFD_NONBLOCK);
    if (eventfd_ == -1) {
        ::munmap(base, alloc_size);
        throw std::system_error(errno, std::generic_category(), "fiber eventfd");
    }

    // 3. Initialize ucontext
    if (::getcontext(&ctx_) == -1) {
        ::munmap(base, alloc_size);
        ::close(eventfd_);
        throw std::system_error(errno, std::generic_category(), "fiber getcontext");
    }

    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stack_size_;
    ctx_.uc_link = nullptr;

    // makecontext needs function pointer with (int, int) signature on all POSIX
    uint32_t id_high = static_cast<uint32_t>(id_ >> 32);
    uint32_t id_low = static_cast<uint32_t>(id_ & 0xFFFFFFFF);
    ::makecontext(&ctx_, reinterpret_cast<void (*)()>(&trampoline), 2, id_high, id_low);
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
        if (g_fiber_storage_deleter_) {
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
    state_.store(FiberState::Running, std::memory_order_release);

    // Swap from worker's loop context to fiber's context
    if (::swapcontext(&wctx->uctx, &ctx_) == -1) {
        std::fprintf(stderr, "fiber[%lu]: resume swapcontext failed: %s\n", (unsigned long)id_,
                     std::strerror(errno));
    }

    // Issue #264: validate yield-boundary checkpoint after resume.
    if (g_fiber_resume_validate_)
        g_fiber_resume_validate_();

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
    if (reason == YieldReason::MutationBoundary &&
        aura::messaging::g_flush_mutation_boundary) {
        aura::messaging::g_flush_mutation_boundary();
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

} // namespace aura::serve

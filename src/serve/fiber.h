// serve/fiber.h — Stackful fiber for async serve
#ifndef AURA_SERVE_FIBER_H
#define AURA_SERVE_FIBER_H

#include <ucontext.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>

// Issue #438: C-linkage forward declaration of the
// per-thread mutation boundary depth probe. Defined
// in evaluator_fiber_mutation.cpp.
extern "C" std::size_t aura_evaluator_mutation_boundary_depth();

// Issue #588: per-fiber mutation stack depth from opaque storage.
// Used by is_at_mutation_boundary_safe() on the victim fiber
// during work-steal (thief thread must not read thread_local).
extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void* mutation_stack_storage);

// Issue #451: C-linkage shim for Fiber's static GC-pause
// counter (defined in fiber.cpp / fiber_bridge.cpp).
extern "C" std::uint64_t aura_fiber_static_gc_pause_attributed_to_mutation();

namespace aura::serve {

// ── Yield reason — why a fiber yielded (Issue #31) ────
// Used by the scheduler to determine if a fiber is at a safe
// point to steal. Only fibers that yielded for Explicit or
// MutationBoundary reasons can be safely stolen — they have
// completed an operation and their state is consistent.
// Fibers in Waiting or BlockingIO have an eventfd pending
// and should not be moved between workers.
enum class YieldReason : uint8_t {
    BlockingIO,        // waiting for external IO (eventfd)
    MutationBoundary,  // yield after completing a mutation/ast:* op
    Explicit,          // explicit yield() call
    SchedulerSteal,    // fiber was stolen by another worker
    OperationBoundary, // yield at sender/receiver boundary (exec adapter)
};

// ── Fiber state ────────────────────────────────────────
enum class FiberState : uint8_t {
    Ready,   // can be scheduled
    Running, // currently executing on a worker
    Waiting, // waiting for eventfd
    Done,    // completed
};

// ── Fiber — stackful coroutine with ucontext ───────────
// Each fiber has its own stack (mmap'd with guard page)
// and an eventfd for scheduler wakeup.
class Fiber {
public:
    using Func = std::function<void()>;

    Fiber(Func func, size_t stack_size = 2 * 1024 * 1024);
    ~Fiber();

    // Non-copyable, movable
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // Switch FROM worker TO this fiber.
    // The worker saves its own loop context via thread_local.
    // After the fiber yields, control returns to the worker's loop.
    void resume();

    // Switch FROM this fiber TO the current worker's loop context.
    // Static — uses thread_local g_worker_ctx.
    static void yield();

    // Yield with reason — allows scheduler to make safe-steal decisions.
    // Fibers that yield with BlockingIO will not be stolen; fibers that
    // yield with MutationBoundary or Explicit are safe to steal.
    static void yield(YieldReason reason);

    // Current yield reason (for the scheduler to inspect)
    YieldReason last_yield_reason() const { return last_yield_reason_; }
    void set_yield_reason(YieldReason r) { last_yield_reason_ = r; }

    // Check if a GC safepoint has been requested (P2).
    // If so, block until the GC phase returns to None.
    // Called from yield() and alloc() paths.
    // Implementation in fiber.cpp (to avoid inline issues with C++26 modules).
    static void check_gc_safepoint();

    // Is this fiber at a safe point to steal/move?
    // A fiber is stealable if it yielded for Explicit, MutationBoundary,
    // or OperationBoundary — meaning it's not in the middle of an
    // inconsistent state.
    bool is_stealable() const {
        auto r = last_yield_reason_.load(std::memory_order_acquire);
        return r == YieldReason::Explicit || r == YieldReason::MutationBoundary ||
               r == YieldReason::OperationBoundary;
    }

    // Issue #448: is_at_safe_mutation_boundary() returns true
    // when this fiber is currently in a state where work-
    // stealing is safe even if a MutationBoundary guard is
    // active. The P0 contract: the fiber is considered
    // "safe" if (a) it has not yielded for a MutationBoundary
    // reason, OR (b) it has yielded for a MutationBoundary
    // but the per-fiber mutation boundary depth is 0
    // (i.e. the outermost guard has already been released).
    //
    // P0: we use a relaxed best-effort check by reading
    // the last_yield_reason_ + a thread_local depth probe.
    // The follow-up will use the proper
    // MutationBoundaryGuard::current_depth() to gate
    // scheduler steal attempts.
    bool is_at_safe_mutation_boundary() const {
        auto r = last_yield_reason_.load(std::memory_order_acquire);
        if (r != YieldReason::MutationBoundary)
            return true;
        // Currently yielded at a MutationBoundary — assume
        // unsafe (the scheduler should defer or skip).
        // The follow-up will use the Evaluator API to
        // probe the per-fiber boundary depth.
        return false;
    }

    // Issue #438: is_at_mutation_boundary_safe() — a more
    // precise version that probes the per-thread mutation
    // boundary depth via a C-linkage function
    // (aura_evaluator_mutation_boundary_depth) defined
    // in evaluator_fiber_mutation.cpp. The C-linkage
    // shim keeps fiber.h free of Evaluator include
    // dependencies (fiber.h is a low-level header
    // included by tests that don't pull in the
    // Evaluator module).
    //
    // Returns true if the fiber is at a safe mutation
    // boundary point (depth == 0 OR the fiber is yielded
    // for a non-MutationBoundary reason).
    bool is_at_mutation_boundary_safe() const {
        auto r = last_yield_reason_.load(std::memory_order_acquire);
        if (r != YieldReason::MutationBoundary)
            return true;
        // Issue #588: read this fiber's stack depth, not the
        // thief thread's thread_local depth.
        return aura_evaluator_mutation_stack_depth_from_ptr(mutation_stack_storage_) == 0;
    }

    // Issue #451: yield_classification() — returns a
    // sub-reason string for the current yield. Used
    // by (query:orchestration-metrics) to break down
    // yields by reason + sub-class. The P0 returns the
    // existing YieldReason as a string; the follow-up
    // adds the sub-reason enum (e.g. MutationBoundary
    // + outermost vs MutationBoundary + inner).
    const char* yield_classification() const {
        switch (last_yield_reason_.load(std::memory_order_acquire)) {
            case YieldReason::BlockingIO:
                return "BlockingIO";
            case YieldReason::MutationBoundary:
                return aura_evaluator_mutation_stack_depth_from_ptr(mutation_stack_storage_) == 0
                           ? "MutationBoundary/outermost"
                           : "MutationBoundary/inner";
            case YieldReason::Explicit:
                return "Explicit";
            case YieldReason::SchedulerSteal:
                return "SchedulerSteal";
            case YieldReason::OperationBoundary:
                return "OperationBoundary";
        }
        return "Unknown";
    }
    // Issue #451: orchestration observability
    // accessors (read by the (query:orchestration-metrics)
    // primitive via the C-linkage shim or via direct
    // Fiber access). All relaxed-ordering, stats-only.
    [[nodiscard]] std::uint64_t yield_mutation_boundary_count() const noexcept {
        return yield_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_explicit_count() const noexcept {
        return yield_explicit_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_scheduler_steal_count() const noexcept {
        return yield_scheduler_steal_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_blocking_io_count() const noexcept {
        return yield_blocking_io_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_operation_boundary_count() const noexcept {
        return yield_operation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t steal_success_count() const noexcept {
        return steal_success_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t steal_deferred_mutation_boundary_count() const noexcept {
        return steal_deferred_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t gc_pause_attributed_to_mutation_count() const noexcept {
        return gc_pause_attributed_to_mutation_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t static_gc_pause_attributed_to_mutation_total() noexcept {
        return static_gc_pause_attributed_to_mutation_count_.load(std::memory_order_relaxed);
    }
    // Issue #451: bump helpers (called by Fiber::yield()
    // + Fiber::check_gc_safepoint() + the work-steal path
    // follow-up).
    void bump_yield_mutation_boundary() noexcept {
        yield_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_explicit() noexcept {
        yield_explicit_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_scheduler_steal() noexcept {
        yield_scheduler_steal_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_blocking_io() noexcept {
        yield_blocking_io_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_operation_boundary() noexcept {
        yield_operation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_steal_success() noexcept {
        steal_success_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_steal_deferred_mutation_boundary() noexcept {
        steal_deferred_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_pause_attributed_to_mutation() noexcept {
        gc_pause_attributed_to_mutation_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Worker affinity (P2): -1 = any worker, 0..N-1 = specific worker
    int affinity() const { return affinity_; }
    void set_affinity(int worker_id) { affinity_ = worker_id; }

    // Accessors
    uint64_t id() const { return id_; }
    FiberState state() const { return state_.load(std::memory_order_acquire); }
    void set_state(FiberState s) { state_.store(s, std::memory_order_release); }
    int eventfd() const { return eventfd_; }
    bool is_done() const { return state_.load(std::memory_order_acquire) == FiberState::Done; }

    // Issue #213 Cycle 3: per-fiber mutation stack. The
    // Evaluator's enter/exit_mutation_boundary reads/writes
    // this stack (via active_mutation_stack()) instead of a
    // thread_local, so a fiber that migrates between threads
    // brings its stack with it.
    //
    // Type: opaque void* to avoid the circular dep between
    // fiber.h and evaluator.ixx. The Evaluator casts it to
    // `std::vector<MutationCheckpoint>*` and operates on
    // it via the pointer.
    void* mutation_stack_ptr() { return mutation_stack_storage_; }
    void set_mutation_stack_ptr(void* p) { mutation_stack_storage_ = p; }
    // Issue #264: per-fiber yield-boundary checkpoint stack.
    void* yield_checkpoint_ptr() { return yield_checkpoint_storage_; }
    void set_yield_checkpoint_ptr(void* p) { yield_checkpoint_storage_ = p; }

private:
    uint64_t id_;
    std::atomic<FiberState> state_{FiberState::Ready};
    std::atomic<YieldReason> last_yield_reason_{YieldReason::Explicit};

    // Issue #451: orchestration observability counters
    // (lifetime atomics, per-Fiber — the follow-up can
    // aggregate to GlobalMetrics for cross-fiber view).
    // Bumped in Fiber::yield() + Fiber::check_gc_safepoint
    // + the work-steal path (follow-up).
    //   yield_mutation_boundary_count_  (lifetime # of
    //     yields with reason == MutationBoundary)
    //   yield_explicit_count_  (lifetime # of yields
    //     with reason == Explicit)
    //   yield_scheduler_steal_count_  (lifetime # of
    //     yields with reason == SchedulerSteal)
    //   yield_blocking_io_count_  (lifetime # of yields
    //     with reason == BlockingIO)
    //   yield_operation_boundary_count_  (lifetime # of
    //     yields with reason == OperationBoundary)
    //   steal_success_count_  (lifetime # of successful
    //     work-steal attempts)
    //   steal_deferred_mutation_boundary_count_  (lifetime
    //     # of steal attempts deferred because the victim
    //     held an outermost MutationBoundary)
    //   gc_pause_attributed_to_mutation_count_  (lifetime
    //     # of GC safepoints where the wait was attributed
    //     to an active MutationBoundary)
    // All stats-only (relaxed-ordering). Exposed via
    // (query:orchestration-metrics).
    std::atomic<std::uint64_t> yield_mutation_boundary_count_{0};
    std::atomic<std::uint64_t> yield_explicit_count_{0};
    std::atomic<std::uint64_t> yield_scheduler_steal_count_{0};
    std::atomic<std::uint64_t> yield_blocking_io_count_{0};
    std::atomic<std::uint64_t> yield_operation_boundary_count_{0};
    std::atomic<std::uint64_t> steal_success_count_{0};
    std::atomic<std::uint64_t> steal_deferred_mutation_boundary_count_{0};
    std::atomic<std::uint64_t> gc_pause_attributed_to_mutation_count_{0};
    int affinity_ = -1; // -1 = any worker, [0,N) = pinned to specific worker
    ucontext_t ctx_;
    void* stack_ = nullptr;
    size_t stack_size_ = 0;
    int eventfd_ = -1;
    Func func_;

    static std::atomic<uint64_t> next_id_;

    // Issue #451: per-Fiber orchestration counters are
    // not appropriate for static-method counters (e.g.
    // gc_pause_attributed_to_mutation is called from
    // the static Fiber::check_gc_safepoint()). Use a
    // static atomic for cross-fiber aggregates. The
    // (query:orchestration-metrics) primitive reads
    // the static aggregate + sums the per-Fiber counters
    // for a process-wide total.
    static std::atomic<std::uint64_t> static_gc_pause_attributed_to_mutation_count_;

    // Trampoline: called when fiber starts
    static void trampoline(uint32_t high, uint32_t low);

    // Per-fiber state: the mutation stack (Issue #213 Cycle 3).
    // Opaque void* — see mutation_stack_ptr() / set_mutation_stack_ptr().
    void* mutation_stack_storage_ = nullptr;
    void* yield_checkpoint_storage_ = nullptr;
};

// Issue #213 Cycle 3: function pointers that the Evaluator
// registers at startup, to avoid the circular include between
// fiber.h and evaluator.ixx. The setter is called by
// Fiber::resume() to update the Evaluator's thread_local
// "current fiber" pointer. The deleter is called by
// ~Fiber() to free the per-fiber storage owned by the
// Evaluator. Both are void(void*) and void*(Fiber*) —
// the function signatures are minimal so the fiber side
// doesn't need to know about Evaluator internals.
extern void* (*g_fiber_setter_)(void*);
// Issue #588: sync per-fiber mutation stack on resume.
extern void (*g_fiber_sync_mutation_stack_)(void*);
extern void (*g_fiber_storage_deleter_)(void*);
// Issue #264: yield-boundary checkpoint hooks (registered by
// evaluator_fiber_mutation.cpp). Called before yield swapcontext
// and after resume swapcontext returns.
extern void (*g_fiber_yield_checkpoint_)(uint8_t reason);
extern void (*g_fiber_resume_validate_)();
extern void (*g_fiber_yield_checkpoint_deleter_)(void*);

// ── GCPhase — GC safepoint state machine (P2) ────────
enum class GCPhase : uint8_t {
    None,      // 正常执行
    Requested, // GC 已请求，等待 fiber 到达安全点
    Sweeping,  // 同步 sweep 进行中
    Complete,  // GC 完成
};

// ── WorkerGCState — per-worker GC state (P2) ──────────
struct WorkerGCState {
    std::atomic<GCPhase> phase{GCPhase::None};
    std::atomic<int32_t> fibers_at_safepoint{0};
    std::atomic<int64_t> gc_epoch{0};

    // Issue #115: count of fibers currently executing on this
    // worker (i.e., the worker is inside fiber->resume() and
    // hasn't returned yet). A fiber that's actively running
    // holds the worker's stack with live references to the
    // heap; the GC must wait for it to either yield or complete
    // before proceeding, otherwise those stack references would
    // be missed during root collection.
    //
    // The fiber's own `check_gc_safepoint` increments
    // `fibers_at_safepoint` when it next yields/allocates.
    // The running-fiber counter is incremented by the worker
    // just before `fiber->resume()` and decremented after.
    // `Scheduler::wait_for_safepoint` considers the worker
    // quiescent only when BOTH counters are zero (or the
    // worker has no fibers at all).
    std::atomic<int32_t> running_fiber_count{0};

    // Spin-wait until phase returns to None (safepoint resume)
    void wait_for_resume() {
        while (phase.load(std::memory_order_acquire) != GCPhase::None) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            asm volatile("" ::: "memory");
#endif
        }
    }
};

// ── Worker context (thread-local) ─────────────────────
// Each WorkerThread sets this before running fibers.
// Fiber::yield() swaps back to this context.
// Fiber::resume() swaps from this context to the fiber.
struct WorkerContext {
    ucontext_t uctx;                   // worker's dispatch loop context
    WorkerGCState* gc_state = nullptr; // set by worker thread (P2)
};
extern thread_local WorkerContext* g_worker_ctx;

// ── Global scheduler reference ─────────────────────────
// Set during --serve-async init. Used for fiber spawn.
struct Scheduler;
extern Scheduler* g_scheduler;
extern thread_local Fiber* g_current_fiber;

} // namespace aura::serve

#endif // AURA_SERVE_FIBER_H

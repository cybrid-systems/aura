// serve/fiber.h — Stackful fiber for async serve
#ifndef AURA_SERVE_FIBER_H
#define AURA_SERVE_FIBER_H

#include <ucontext.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>

namespace aura::serve {

// ── Yield reason — why a fiber yielded (Issue #31) ────
// Used by the scheduler to determine if a fiber is at a safe
// point to steal. Only fibers that yielded for Explicit or
// MutationBoundary reasons can be safely stolen — they have
// completed an operation and their state is consistent.
// Fibers in Waiting or BlockingIO have an eventfd pending
// and should not be moved between workers.
enum class YieldReason : uint8_t {
    BlockingIO,         // waiting for external IO (eventfd)
    MutationBoundary,   // yield after completing a mutation/ast:* op
    Explicit,           // explicit yield() call
    SchedulerSteal,     // fiber was stolen by another worker
    OperationBoundary,  // yield at sender/receiver boundary (exec adapter)
};

// ── Fiber state ────────────────────────────────────────
enum class FiberState : uint8_t {
    Ready,    // can be scheduled
    Running,  // currently executing on a worker
    Waiting,  // waiting for eventfd
    Done,     // completed
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

    // Is this fiber at a safe point to steal/move?
    // A fiber is stealable if it yielded for Explicit, MutationBoundary,
    // or OperationBoundary — meaning it's not in the middle of an
    // inconsistent state.
    bool is_stealable() const {
        auto r = last_yield_reason_.load(std::memory_order_acquire);
        return r == YieldReason::Explicit ||
               r == YieldReason::MutationBoundary ||
               r == YieldReason::OperationBoundary;
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

private:
    uint64_t id_;
    std::atomic<FiberState> state_{FiberState::Ready};
    std::atomic<YieldReason> last_yield_reason_{YieldReason::Explicit};
    int affinity_ = -1;  // -1 = any worker, [0,N) = pinned to specific worker
    ucontext_t ctx_;
    void* stack_ = nullptr;
    size_t stack_size_ = 0;
    int eventfd_ = -1;
    Func func_;

    static std::atomic<uint64_t> next_id_;

    // Trampoline: called when fiber starts
    static void trampoline(uint32_t high, uint32_t low);
};

// ── Worker context (thread-local) ─────────────────────
// Each WorkerThread sets this before running fibers.
// Fiber::yield() swaps back to this context.
// Fiber::resume() swaps from this context to the fiber.
struct WorkerContext {
    ucontext_t uctx;  // worker's dispatch loop context
};
extern thread_local WorkerContext* g_worker_ctx;

// ── Global scheduler reference ─────────────────────────
// Set during --serve-async init. Used for fiber spawn.
struct Scheduler;
extern Scheduler* g_scheduler;
extern thread_local Fiber* g_current_fiber;

} // namespace aura::serve

#endif // AURA_SERVE_FIBER_H

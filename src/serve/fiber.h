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

// ── Fiber state ────────────────────────────────────────
enum class FiberState : uint8_t {
    Ready,    // can be scheduled
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

    // Switch FROM scheduler TO this fiber
    void resume();

    // Switch FROM this fiber TO scheduler (static — yields current fiber)
    static void yield();

    // Accessors
    uint64_t id() const { return id_; }
    FiberState state() const { return state_; }
    void set_state(FiberState s) { state_ = s; }
    int eventfd() const { return eventfd_; }
    bool is_done() const { return state_ == FiberState::Done; }

private:
    uint64_t id_;
    FiberState state_ = FiberState::Ready;
    ucontext_t ctx_;
    void* stack_ = nullptr;
    size_t stack_size_ = 0;
    int eventfd_ = -1;
    Func func_;

    static std::atomic<uint64_t> next_id_;

    // Trampoline: called when fiber starts
    static void trampoline(uint32_t high, uint32_t low);
};

// ── Global scheduler reference ─────────────────────────
// Set during --serve-async init. Used by Fiber::yield().
struct Scheduler;
extern Scheduler* g_scheduler;
extern thread_local Fiber* g_current_fiber;

} // namespace aura::serve

#endif // AURA_SERVE_FIBER_H

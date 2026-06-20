// exec/execution_adapter.h — C++26 std::execution-inspired fiber scheduler adapter
//
// Lightweight senders/receivers pattern for Aura's M:N fiber scheduler.
// Wraps aura::serve::Scheduler into a composable async execution context.
//
// Key types:
//   fiber_scheduler  — std::execution::scheduler-like wrapper around Scheduler
//   fiber_sender     — represents "schedule work on this scheduler"
//   operation_state  — connects sender to receiver; call start() to begin
//
// This is NOT a full P2300 implementation. GCC 16's <execution> is still
// experimental. Instead, we define a minimal set of custom tag types
// that follow the sender/receiver semantic without depending on the TS.
//
// Architecture (Issue #33):
//   Aura primitives (orch:parallel, orch:pipeline)
//       ↓
//   aura::exec::fiber_scheduler      ← lightweight sender/receiver
//       ↓
//   aura::serve::Scheduler            ← existing M:N work-stealing scheduler
//       ↓
//   WorkerThread [...]                ← existing Chase-Lev deques
//
#ifndef AURA_EXEC_EXECUTION_ADAPTER_H
#define AURA_EXEC_EXECUTION_ADAPTER_H

#include <functional>
#include <memory>
#include <chrono>
#include <exception>
#include <optional>
#include <atomic>
#include <vector>

namespace aura::serve {
class Scheduler;
}

namespace aura::exec {

// ── Forward declarations ─────────────────────────────
class fiber_scheduler;
class fiber_sender;
class operation_state;

// ── Execution tags (lightweight alternative to P2300 tags) ──
struct set_value_t {};   // operation completed successfully
struct set_error_t {};   // operation completed with error
struct set_stopped_t {}; // operation was cancelled

inline constexpr set_value_t set_value{};
inline constexpr set_error_t set_error{};
inline constexpr set_stopped_t set_stopped{};

// ── fiber_receiver — receives completion signals ─────
// The receiver is called when the operation completes.
// It has three entry points: set_value, set_error, set_stopped.
// For simplicity, we use a std::function-based receiver.
class fiber_receiver {
public:
    using value_fn = std::function<void()>;
    using error_fn = std::function<void(std::exception_ptr)>;
    using stopped_fn = std::function<void()>;

    fiber_receiver() = default;

    // Construct with all three callbacks
    fiber_receiver(value_fn v, error_fn e, stopped_fn s)
        : on_value_(std::move(v))
        , on_error_(std::move(e))
        , on_stopped_(std::move(s)) {}

    // Called when operation succeeds
    void set_value() {
        if (on_value_)
            on_value_();
    }

    // Called when operation fails
    void set_error(std::exception_ptr e) {
        if (on_error_)
            on_error_(std::move(e));
    }

    // Called when operation is cancelled
    void set_stopped() {
        if (on_stopped_)
            on_stopped_();
    }

    // Check if receiver is valid
    explicit operator bool() const { return static_cast<bool>(on_value_); }

private:
    value_fn on_value_;
    error_fn on_error_;
    stopped_fn on_stopped_;
};

// ── operation_state — represents an in-flight async operation ──
// Created by connect(sender, receiver). Started by start(state).
// The state object must live until completion.
class operation_state {
public:
    operation_state() = default;

    // Non-copyable, movable
    operation_state(const operation_state&) = delete;
    operation_state& operator=(const operation_state&) = delete;
    operation_state(operation_state&&) = default;
    operation_state& operator=(operation_state&&) = default;

    // Begin execution. The receiver will be signalled upon completion.
    // After start(), the caller must keep this state alive until
    // the receiver fires.
    void start();

    // Access the wrapped fiber ID (for join/tracking)
    int64_t fiber_id() const { return fiber_id_; }

private:
    friend class fiber_sender;
    friend class fiber_scheduler;

    aura::serve::Scheduler* scheduler_ = nullptr;
    std::function<void()> fn_;
    fiber_receiver receiver_;
    int64_t fiber_id_ = -1;
    bool started_ = false;
};

// ── fiber_sender — a sender that spawns work on the fiber scheduler ──
// Created by fiber_scheduler::schedule(fn).
// Caller connects it to a receiver, then starts the operation.
class fiber_sender {
public:
    fiber_sender() = default;

    fiber_sender(aura::serve::Scheduler* sched, std::function<void()> fn)
        : scheduler_(sched)
        , fn_(std::move(fn)) {}

    // Connect this sender to a receiver. Returns an operation_state.
    // The caller must keep the state alive until the receiver fires.
    operation_state connect(fiber_receiver rcvr) && {
        operation_state state;
        state.scheduler_ = scheduler_;
        state.fn_ = std::move(fn_);
        state.receiver_ = std::move(rcvr);
        return state;
    }

private:
    friend class fiber_scheduler;
    aura::serve::Scheduler* scheduler_ = nullptr;
    std::function<void()> fn_;
};

// ── fiber_scheduler — std::execution::scheduler-like wrapper ──
// Wraps aura::serve::Scheduler for use with the sender/receiver pattern.
class fiber_scheduler {
public:
    explicit fiber_scheduler(aura::serve::Scheduler& sched)
        : sched_(&sched) {}

    // Create a sender that will schedule fn on the fiber scheduler.
    fiber_sender schedule(std::function<void()> fn) { return fiber_sender(sched_, std::move(fn)); }

    // Access the underlying scheduler
    aura::serve::Scheduler& underlying() { return *sched_; }
    const aura::serve::Scheduler& underlying() const { return *sched_; }

private:
    aura::serve::Scheduler* sched_;
};

// ── Convenience: create a scheduler from a Scheduler reference ──
inline fiber_scheduler make_fiber_scheduler(aura::serve::Scheduler& sched) {
    return fiber_scheduler(sched);
}

} // namespace aura::exec

#endif // AURA_EXEC_EXECUTION_ADAPTER_H

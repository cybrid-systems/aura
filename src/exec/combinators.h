// exec/combinators.h — Structured concurrency combinators
//
// Send/receiver combinators built on top of fiber_scheduler:
//   when_all     — run N operations in parallel, wait for all
//   let_value    — chain operations sequentially (pipeline)
//   with_timeout — cancel an operation after a deadline
//   retry        — retry an operation on failure up to N times
//
// These are the building blocks for Aura Layer 3 orchestration:
//   orch:parallel  ← when_all
//   orch:pipeline  ← let_value chain
//   orch:timeout   ← with_timeout
//
#ifndef AURA_EXEC_COMBINATORS_H
#define AURA_EXEC_COMBINATORS_H

#include "execution_adapter.h"

#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>

namespace aura::exec {

// ── when_all — parallel composition ──────────────────
//
// Spawns N fibers in parallel (each on the given scheduler).
// The returned sender completes when ALL fibers finish.
// If any fiber fails, the error is propagated.
//
// Usage:
//   auto op = when_all(sched, {fn1, fn2, fn3});
//   auto state = std::move(op).connect(receiver);
//   state.start();
//

class when_all_sender {
public:
    using Fn = std::function<void()>;

    when_all_sender(fiber_scheduler& sched, std::vector<Fn> fns)
        : sched_(&sched)
        , fns_(std::move(fns)) {}

    operation_state connect(fiber_receiver rcvr) &&;

private:
    fiber_scheduler* sched_;
    std::vector<Fn> fns_;
};

// ── let_value — sequential composition (pipeline) ────
//
// Runs fn1, then fn2, then fn3 sequentially on the same scheduler.
// Each function receives no input (simplified: the pipeline is
// ordered execution, not dataflow).
// If any step fails, the pipeline stops and the error propagates.
//
// Usage:
//   auto op = let_value(sched, {fn1, fn2, fn3});
//   auto state = std::move(op).connect(receiver);
//   state.start();
//

class let_value_sender {
public:
    using Fn = std::function<void()>;

    let_value_sender(fiber_scheduler& sched, std::vector<Fn> fns)
        : sched_(&sched)
        , fns_(std::move(fns)) {}

    operation_state connect(fiber_receiver rcvr) &&;

private:
    fiber_scheduler* sched_;
    std::vector<Fn> fns_;
};

// ── with_timeout — cancellation after deadline ───────
//
// Runs fn on the scheduler. If it doesn't complete within
// 'timeout', the operation is cancelled (receiver gets set_stopped).
// The fiber is NOT forcibly killed — it continues running but
// the caller is unblocked. (Cooperative cancellation model.)
//
// Usage:
//   auto op = with_timeout(sched, fn, std::chrono::seconds(5));
//   auto state = std::move(op).connect(receiver);
//   state.start();
//

class with_timeout_sender {
public:
    using Fn = std::function<void()>;

    with_timeout_sender(fiber_scheduler& sched, Fn fn, std::chrono::milliseconds timeout)
        : sched_(&sched)
        , fn_(std::move(fn))
        , timeout_(timeout) {}

    operation_state connect(fiber_receiver rcvr) &&;

private:
    fiber_scheduler* sched_;
    Fn fn_;
    std::chrono::milliseconds timeout_;
};

// ── retry — retry on failure ─────────────────────────
//
// Runs fn up to `max_attempts` times. If all succeed, complete.
// If all fail, propagate the last error.
//
// Usage:
//   auto op = retry(sched, fn, 3);
//   auto state = std::move(op).connect(receiver);
//   state.start();
//

class retry_sender {
public:
    using Fn = std::function<void()>;

    retry_sender(fiber_scheduler& sched, Fn fn, int max_attempts)
        : sched_(&sched)
        , fn_(std::move(fn))
        , max_attempts_(max_attempts) {}

    operation_state connect(fiber_receiver rcvr) &&;

private:
    fiber_scheduler* sched_;
    Fn fn_;
    int max_attempts_;
};

// ── Free-function wrappers ───────────────────────────

inline when_all_sender when_all(fiber_scheduler& sched, std::vector<std::function<void()>> fns) {
    return when_all_sender(sched, std::move(fns));
}

inline let_value_sender let_value(fiber_scheduler& sched, std::vector<std::function<void()>> fns) {
    return let_value_sender(sched, std::move(fns));
}

inline with_timeout_sender with_timeout(fiber_scheduler& sched, std::function<void()> fn,
                                        std::chrono::milliseconds timeout) {
    return with_timeout_sender(sched, std::move(fn), timeout);
}

inline retry_sender retry(fiber_scheduler& sched, std::function<void()> fn, int max_attempts) {
    return retry_sender(sched, std::move(fn), max_attempts);
}

} // namespace aura::exec

#endif // AURA_EXEC_COMBINATORS_H

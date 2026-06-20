// exec/combinators.cpp — Implementation of combinator connection logic
#include "combinators.h"
#include "serve/scheduler.h"
#include "serve/fiber.h"

#include <cstdio>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

namespace aura::exec {

// ── when_all_sender implementation ───────────────────
//
// when_all spawns all fibers up front, then waits for all to complete.
// Uses an atomic counter to track completion.
// When the counter reaches 0, the receiver's set_value is called.
// If any fiber sets an error, the error is stored and propagated.
//
operation_state when_all_sender::connect(fiber_receiver rcvr) && {
    // Build a shared completion state
    struct shared_state {
        std::atomic<int> remaining{0};
        std::atomic<bool> has_error{false};
        std::exception_ptr error;
        fiber_receiver receiver;
    };

    auto state = std::make_shared<shared_state>();
    state->remaining.store(static_cast<int>(fns_.size()), std::memory_order_release);
    state->receiver = std::move(rcvr);

    // Spawn each fiber
    for (auto& fn : fns_) {
        auto sched_ref = *sched_; // copy the scheduler
        auto fn_copy = std::move(fn);
        auto state_copy = state;

        auto sender = sched_->schedule([fn_copy = std::move(fn_copy), state_copy]() mutable {
            try {
                fn_copy();
            } catch (...) {
                // Store the first error
                auto expected = false;
                if (state_copy->has_error.compare_exchange_strong(expected, true,
                                                                  std::memory_order_acq_rel)) {
                    state_copy->error = std::current_exception();
                }
            }

            // Decrement the counter
            int prev = state_copy->remaining.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                // All done — signal the receiver
                if (state_copy->has_error.load(std::memory_order_acquire)) {
                    state_copy->receiver.set_error(state_copy->error);
                } else {
                    state_copy->receiver.set_value();
                }
            }
        });

        auto op_state = std::move(sender).connect(fiber_receiver{});
        op_state.start();
    }

    // Return a dummy operation_state (the real work is managed by shared_state)
    operation_state dummy;
    return dummy;
}

// ── let_value_sender implementation ──────────────────
//
// let_value runs fibers sequentially: fn1 → fn2 → fn3.
// Each step runs to completion before the next starts.
// If any step fails, subsequent steps are skipped.
//
// Implementation: spawn a single fiber that runs all functions
// in sequence. This keeps the sequential semantics simple.
//
operation_state let_value_sender::connect(fiber_receiver rcvr) && {
    auto sender = sched_->schedule([fns = std::move(fns_)]() {
        for (auto& fn : fns) {
            fn();
        }
    });

    auto op_state = std::move(sender).connect(std::move(rcvr));
    return op_state;
}

// ── with_timeout_sender implementation ───────────────
//
// Spawns the fiber normally, but also starts a timer thread
// that calls set_stopped if the fiber doesn't complete in time.
// The receiver must handle both set_value and set_stopped.
//
// NOTE: This is a COOPERATIVE cancel model. The fiber continues
// running, but the caller is unblocked. If the fiber later completes,
// its set_value is silently ignored (the receiver is already done).
//
operation_state with_timeout_sender::connect(fiber_receiver rcvr) && {
    struct shared_state {
        std::atomic<bool> done{false};
        fiber_receiver receiver;
    };

    auto state = std::make_shared<shared_state>();
    state->receiver = std::move(rcvr);

    // Spawn the fiber
    auto sender = sched_->schedule([fn = std::move(fn_), state]() mutable {
        try {
            fn();
            // Check if the timeout already fired
            auto expected = false;
            if (state->done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                state->receiver.set_value();
            }
        } catch (...) {
            auto expected = false;
            if (state->done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                state->receiver.set_error(std::current_exception());
            }
        }
    });

    auto op_state = std::move(sender).connect(fiber_receiver{});
    op_state.start();

    // Start timeout thread
    std::thread([state, timeout = timeout_]() {
        std::this_thread::sleep_for(timeout);
        auto expected = false;
        if (state->done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            state->receiver.set_stopped();
        }
    }).detach();

    return operation_state{};
}

// ── retry_sender implementation ──────────────────────
//
// Runs fn up to max_attempts times. On success, complete.
// On failure, retry. If all attempts fail, propagate the last error.
//
operation_state retry_sender::connect(fiber_receiver rcvr) && {
    struct shared_state {
        int max_attempts;
        std::function<void()> fn;
        fiber_receiver receiver;
    };

    auto state = std::make_shared<shared_state>();
    state->max_attempts = max_attempts_;
    state->fn = std::move(fn_);
    state->receiver = std::move(rcvr);

    // Spawn a fiber that loops with retries
    auto sender = sched_->schedule([state]() {
        std::exception_ptr last_error;
        for (int attempt = 0; attempt < state->max_attempts; ++attempt) {
            try {
                state->fn();
                state->receiver.set_value();
                return;
            } catch (...) {
                last_error = std::current_exception();
                // Brief yield before retry to allow other fibers to run
                if (attempt + 1 < state->max_attempts) {
                    aura::serve::Fiber::yield();
                }
            }
        }
        // All attempts failed
        state->receiver.set_error(last_error);
    });

    auto op_state = std::move(sender).connect(fiber_receiver{});
    op_state.start();
    return operation_state{};
}

} // namespace aura::exec

// exec/execution_adapter.cpp — Implementation of sender/receiver adapter
#include "execution_adapter.h"
#include "serve/scheduler.h"
#include "serve/fiber.h"  // YieldReason

#include <cstdio>
#include <cerrno>
#include <cstring>

namespace aura::exec {

// ── operation_state::start — begin execution ─────────
//
// Spawns a fiber that runs the user function. When the function
// returns (or throws), the appropriate receiver callback fires.
// The fiber's eventfd fires to wake the IO thread, which enqueues
// any waiting joiner.
//
void operation_state::start() {
    if (started_ || !scheduler_ || !fn_) {
        return;  // nothing to do
    }
    started_ = true;

    // Capture receiver (moved into the fiber lambda)
    auto rcvr = std::move(receiver_);

    // Spawn the fiber
    fiber_id_ = static_cast<int64_t>(
        reinterpret_cast<intptr_t>(
            scheduler_->spawn([fn = std::move(fn_), rcvr = std::move(rcvr)]() mutable {
                // Yield at operation boundary (safe point before work)
                aura::serve::Fiber::yield(aura::serve::YieldReason::OperationBoundary);

                try {
                    fn();

                    // Yield at operation boundary after completion.
                    // This marks the fiber as stealable at a safe point,
                    // ensuring the scheduler can move it between workers
                    // only when its state is consistent (Issue #31).
                    aura::serve::Fiber::yield(aura::serve::YieldReason::OperationBoundary);

                    // Signal completion via the receiver
                    rcvr.set_value();
                } catch (...) {
                    rcvr.set_error(std::current_exception());
                }
            })
        )
    );
}

} // namespace aura::exec

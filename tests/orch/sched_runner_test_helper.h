// Test helper for `aura::serve::SchedRunner`.
//
// GCC 16.0.1 silently accepted `using aura::serve::SchedRunner;` even though
// the class didn't exist (looser two-phase name lookup). GCC 16.1.0 enforces
// strict ODR lookup and rejects it. The class is a test-only RAII wrapper
// (start a worker thread running a Scheduler on construction, stop + join
// on destruction) — not production code.
//
// 4 test files use it:
//   - tests/orch/test_join_drain_reclaim.cpp
//   - tests/orch/test_mailbox_bp_admit.cpp
//   - tests/orch/test_agent_failure_policy.cpp
//   - tests/orch/test_agent_ask.cpp
//
// (test_agent_ask originally had `aura::orch::SchedRunner` — wrong
// namespace; #2288 build fix updates it to `aura::serve::SchedRunner`.)
#pragma once

#include "serve/scheduler.h" // full def — .run() / .stop() need the full type
#include <thread>

namespace aura::serve {

class SchedRunner {
public:
    explicit SchedRunner(Scheduler& s)
        : sched_(s) {
        thread_ = std::thread([this] { sched_.run(); });
    }

    ~SchedRunner() {
        sched_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    SchedRunner(const SchedRunner&) = delete;
    SchedRunner& operator=(const SchedRunner&) = delete;
    SchedRunner(SchedRunner&&) = delete;
    SchedRunner& operator=(SchedRunner&&) = delete;

private:
    Scheduler& sched_;
    std::thread thread_;
};

} // namespace aura::serve

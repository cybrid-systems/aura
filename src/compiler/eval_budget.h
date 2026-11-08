// Issue #5042: a step budget and an interrupt flag the REPL can arm around a
// candidate evaluation. The evaluator safepoint poll consumes one step per
// poll; when the budget runs out or Ctrl-C was pressed, evaluation returns a
// diagnostic and the caller rolls the proposal back.
#pragma once

#include <atomic>

namespace aura::compiler::eval_budget {

inline std::atomic<long> g_fuel{-1}; // -1: no budget armed
inline std::atomic<bool> g_interrupt{false};

inline void arm(long steps) {
    g_interrupt.store(false, std::memory_order_relaxed);
    g_fuel.store(steps, std::memory_order_relaxed);
}

inline void disarm() {
    g_fuel.store(-1, std::memory_order_relaxed);
    g_interrupt.store(false, std::memory_order_relaxed);
}

inline void request_interrupt() {
    g_interrupt.store(true, std::memory_order_relaxed);
}

// True when the armed budget is spent or an interrupt was requested.
inline bool consume_or_interrupted() noexcept {
    if (g_interrupt.load(std::memory_order_relaxed))
        return true;
    const long left = g_fuel.load(std::memory_order_relaxed);
    if (left < 0)
        return false;
    if (left == 0)
        return true;
    g_fuel.store(left - 1, std::memory_order_relaxed);
    return false;
}

} // namespace aura::compiler::eval_budget

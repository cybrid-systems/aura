// Issue #4617: cross-direction GC roots between the IR interpreter and the
// tree-walker's file-mode reclaim.
//
// #4481 made the IR sweep absorb the tree-walker's in-flight roots
// (Evaluator::collect_tw_inflight_roots). The reverse direction was
// missing: IR code that calls a tree-walked file-level procedure by name
// is suspended mid-frame while that procedure's eval loop reaches
// Evaluator::maybe_reclaim_tw_unreachable, whose root walk only knows
// g_tw_frames / g_tw_results / g_tw_vecs / g_tw_spans. The caller's IR
// locals, args, linear slots, cell contents and const string/keyword
// caches were freed underneath it: a local vector then reads length 0 and
// a local list's cells read ().
//
// A module import cannot express this (ir_executor imports evaluator; the
// evaluator cannot import ir_executor back), so this header — included in
// both global module fragments — carries a thread-local provider stack.
// The IR driver installs a collector for the duration of its drain; the
// tree-walker sweep asks for the innermost live set.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::compiler::ir_inflight {

// Raw EvalValue bit patterns (#907: one tagged int64). The evaluator wraps
// them back into types::EvalValue, so this header needs no value type and
// neither module sees the other's declarations.
using Collector = void (*)(void* ctx, std::vector<std::int64_t>& out);

// Issue #4816: an explicit (gc-heap) under --load asks the innermost IR
// driver on this thread to run its file-heap sweep now, at the primitive
// call (a safepoint: the caller's frame values are in call_stack_). Returns
// 1 = swept, 0 = stood down (a foreign body / drain could not be certified).
using Reclaimer = int (*)(void* ctx);

struct Provider {
    Collector collect = nullptr;
    void* ctx = nullptr;
    Reclaimer reclaim = nullptr;
    // Issue #4915: the captured env values of every IR closure the driver
    // holds. Only the tree-walker sweep asks for them: it cannot see IR
    // closures, so a pair or walker closure reachable only through an IR
    // closure env would otherwise be unmarked.
    Collector collect_closure_envs = nullptr;
};

// Thread-local, mirroring IRInterpreter::t_active_'s nesting: a deopt
// resume may re-enter an instance that is already draining.
inline thread_local std::vector<Provider> g_providers;

// RAII registration. Nested scopes pop in reverse order (LIFO), like
// TwFrameReg / ActiveInstanceGuard.
struct ScopedProvider {
    ScopedProvider(Collector collect, void* ctx, Reclaimer reclaim = nullptr,
                   Collector collect_closure_envs = nullptr) noexcept {
        g_providers.push_back(Provider{collect, ctx, reclaim, collect_closure_envs});
    }
    ~ScopedProvider() {
        if (!g_providers.empty())
            g_providers.pop_back();
    }
    ScopedProvider(const ScopedProvider&) = delete;
    ScopedProvider& operator=(const ScopedProvider&) = delete;
};

// Appends every live provider's roots to `out`. Returns the number of
// values appended (0 = no IR driver is on this thread's stack).
inline std::size_t collect(std::vector<std::int64_t>& out) {
    const auto before = out.size();
    for (const auto& p : g_providers) {
        if (p.collect != nullptr)
            p.collect(p.ctx, out);
    }
    return out.size() - before;
}

// Issue #4915: the IR closure env values of every driver on this thread.
inline std::size_t collect_closure_envs(std::vector<std::int64_t>& out) {
    const auto before = out.size();
    for (const auto& p : g_providers) {
        if (p.collect_closure_envs != nullptr)
            p.collect_closure_envs(p.ctx, out);
    }
    return out.size() - before;
}

// Issue #4915: how many distinct IR interpreters are draining on this
// thread's stack (a deopt resume can re-enter one instance, so nested
// entries with the same ctx count once - the same unit as the process-wide
// aura_ir_active_drains_total count).
inline int distinct_drivers() {
    int n = 0;
    for (std::size_t i = 0; i < g_providers.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (g_providers[j].ctx == g_providers[i].ctx) {
                seen = true;
                break;
            }
        }
        if (!seen)
            ++n;
    }
    return n;
}

// Issue #4816: run the innermost IR driver's sweep. -1 = no IR driver with a
// reclaimer on this thread (the caller falls back to the tree-walker sweep).
inline int reclaim_innermost() {
    for (auto it = g_providers.rbegin(); it != g_providers.rend(); ++it) {
        if (it->reclaim != nullptr)
            return it->reclaim(it->ctx);
    }
    return -1;
}

} // namespace aura::compiler::ir_inflight

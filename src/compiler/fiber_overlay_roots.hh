// Issue #4849: GC roots for pending COW-overlay writes.
//
// (mutate :overlay-begin) ... (mutate :overlay-define / set! / write) store
// their pending values ONLY in the process-level OverlayRegistry until
// fiber:overlay-commit publishes them into the shared env. Neither the
// tree-walker reclaim nor the file-mode reclaim could name that registry,
// so a large pending list was marked unreachable, freed, and reused by the
// churn allocation: the commit then published (()) or another object's
// data. The overlay pending write set is a root for exactly the same
// reason the fiber:join result registry is (fiber_result_roots.hh).
//
// A module import cannot express the dependency (the COW partition already
// imports the evaluator), so this header carries a provider that the COW
// partition installs once and the reclaims consult.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::compiler::fiber_overlay_roots {

// Raw EvalValue bit patterns (one tagged int64); see fiber_result_roots.hh.
using Collector = std::size_t (*)(std::vector<std::int64_t>& out);

// Process-global: the overlay registry is process-global and partitioned by
// Evaluator pointer, so every reclaim must see every evaluator's pending
// writes, not only its own thread's.
inline std::atomic<Collector> g_collector{nullptr};

inline void install(Collector collect) noexcept {
    Collector expected = nullptr;
    (void)g_collector.compare_exchange_strong(expected, collect, std::memory_order_release,
                                              std::memory_order_relaxed);
}

inline std::size_t collect(std::vector<std::int64_t>& out) {
    const auto before = out.size();
    const Collector collect_fn = g_collector.load(std::memory_order_acquire);
    if (collect_fn != nullptr)
        collect_fn(out);
    return out.size() - before;
}

} // namespace aura::compiler::fiber_overlay_roots

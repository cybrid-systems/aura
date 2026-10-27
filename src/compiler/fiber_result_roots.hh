// Issue #4710: GC roots for the fiber:spawn result registry.
//
// A fiber body's return value is stored in s_fiber_results (the
// fiber:join registry, evaluator_primitives_messaging.cpp) and lives
// ONLY there until fiber:join fetches and erases the entry. That
// window — fiber finished, entry published, join not yet run — is
// fully reachable from the program's point of view, but neither the
// tree-walker reclaim (Evaluator::reclaim_tw_unreachable) nor the
// file-mode reclaim (Evaluator::reclaim_file_mode_unreachable) could
// see it: the registry is TU-local to the messaging partition.
//
// The reclaim therefore proved a (hash) table of a finished fiber's
// result unreachable, freed it, put the slot on the hash free list,
// and the next (hash) mint reused the slot. The joiner then read an
// empty table (-1) or another fiber's table (two identical values).
//
// A module import cannot express the dependency (the messaging
// partition already imports the evaluator), so this header — included
// in both global module fragments — carries a provider that the
// messaging partition installs once and the reclaims consult.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::compiler::fiber_result_roots {

// Raw EvalValue bit patterns (#907: one tagged int64). The evaluator
// wraps them back into types::EvalValue, so this header needs no value
// type and neither partition sees the other's declarations.
using Collector = std::size_t (*)(std::vector<std::int64_t>& out);

// Process-global: the fiber result registry is process-global too (the
// result of a finished fiber is visible to a joiner on ANY thread), so
// the provider is not thread-local. A reclaim that frees a result must
// see the fibers of every thread — not only its own (the #4617
// IR/TW providers are the opposite: those roots are strictly
// thread-local).
inline std::atomic<Collector> g_collector{nullptr};

// First install wins. Primitive registration runs per Evaluator (several
// in a serve session); the registry is one map, so one provider serves
// all of them.
inline void install(Collector collect) noexcept {
    Collector expected = nullptr;
    (void)g_collector.compare_exchange_strong(expected, collect, std::memory_order_release,
                                              std::memory_order_relaxed);
}

// Appends every live fiber result to out. Returns the number of values
// appended (0 = no provider installed, e.g. a non-messaging embedding).
inline std::size_t collect(std::vector<std::int64_t>& out) {
    const auto before = out.size();
    const Collector collect_fn = g_collector.load(std::memory_order_acquire);
    if (collect_fn != nullptr)
        collect_fn(out);
    return out.size() - before;
}

} // namespace aura::compiler::fiber_result_roots

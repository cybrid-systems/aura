// #5065: GC roots for engine:metrics key strings interned once per heap.
//
// A full snapshot used to mint a fresh string for every metric field on
// every call (~2350 slots, each taking the string-heap lock). The intern
// table keeps those slots, and the reclaims must see them or the next
// snapshot would observe a reused slot and rebuild the table anyway.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::compiler::metrics_intern_roots {

using Collector = std::size_t (*)(std::vector<std::int64_t>& out);

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

} // namespace aura::compiler::metrics_intern_roots

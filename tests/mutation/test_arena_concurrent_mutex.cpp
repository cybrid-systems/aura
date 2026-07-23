// test_arena_concurrent_mutex.cpp — Issue #1988: ArenaGroup::arenas_ concurrent access.
//
// Single-case stress test: N writer threads + M reader threads concurrently
// call module_arena / count. Without arenas_mtx_ this is UB iterator
// invalidation (concurrent emplace + iteration on std::unordered_map).
// With the fix the map is protected by a shared_mutex (exclusive for
// writers, shared for readers) and the test runs to completion without
// crashing.

#include "test_harness.hpp"

#include <atomic>
#include <format>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.arena;

namespace {
constexpr int kWriterThreads = 4;
constexpr int kReaderThreads = 2;
constexpr int kItersPerWriter = 50;
constexpr int kKeysPerIter = 8;
} // namespace

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    aura::ast::ArenaGroup group;

    // Pre-create some arenas so readers/writers have something to race against.
    for (int i = 0; i < 4; ++i) {
        (void)group.module_arena(std::format("pre_{}", i));
    }
    CHECK(group.count() == 4, "pre-created 4 arenas");

    std::atomic<bool> writers_done{false};
    std::atomic<int> write_errors{0};

    // Writer threads: insert arenas with random keys concurrently.
    std::vector<std::thread> writers;
    writers.reserve(kWriterThreads);
    for (int t = 0; t < kWriterThreads; ++t) {
        writers.emplace_back([&, t]() noexcept {
            std::mt19937 rng(static_cast<std::uint32_t>(t) + 1u);
            std::uniform_int_distribution<int> dist(0, 999);
            for (int i = 0; i < kItersPerWriter; ++i) {
                for (int k = 0; k < kKeysPerIter; ++k) {
                    try {
                        (void)group.module_arena(std::format("w{}_{}", t, dist(rng)));
                    } catch (...) {
                        ++write_errors;
                    }
                }
            }
        });
    }

    // Reader threads: repeatedly read count() (shared_lock on arenas_mtx_).
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (int t = 0; t < kReaderThreads; ++t) {
        readers.emplace_back([&]() noexcept {
            while (!writers_done.load(std::memory_order_acquire)) {
                const auto n = group.count();
                if (n < 4)
                    ++write_errors; // concurrent reader saw a torn state
            }
        });
    }

    for (auto& w : writers)
        w.join();
    writers_done.store(true, std::memory_order_release);
    for (auto& r : readers)
        r.join();

    // Verify final state: >= pre-created count, no exceptions under load.
    const auto final_count = group.count();
    CHECK(final_count >= 4, "at least 4 arenas after concurrent stress");
    CHECK(write_errors.load() == 0, "no write errors or torn reads under concurrent load");

    if (::aura::test::g_failed)
        return 1;
    std::println("arena concurrent mutex #1988: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

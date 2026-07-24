// test_invalidate_cascade_order.cpp — Issue #1378:
// invalidate_function: epoch bump + dirty under mutate_lock;
// jit_.invalidate under same jit_cache_mtx_ as erase.

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;

using aura::compiler::CompilerService;

int main() {
    // ── AC1: single-thread epoch + calls counters ──
    // Note: mutation_epoch_ may advance more than once per invalidate
    // (entry bump under lock + optional bump_bridge_epoch via
    // invalidate_shape). The #1378 contract is: calls counter is exact
    // (no re-entrant double-entry), and epoch is strictly monotonic.
    {
        CompilerService cs;
        const auto e0 = cs.public_mutation_epoch();
        const auto c0 = cs.public_invalidate_function_calls();
        cs.public_record_dependency("g", "f");
        cs.public_record_dependency("h", "g");
        cs.public_invalidate_function("f");
        CHECK(cs.public_invalidate_function_calls() == c0 + 1, "calls +1 per invalidate");
        CHECK(cs.public_mutation_epoch() >= e0 + 1, "epoch advanced >= 1 after invalidate");
        const auto e1 = cs.public_mutation_epoch();
        cs.public_invalidate_function("f");
        CHECK(cs.public_invalidate_function_calls() == c0 + 2, "calls +2 after second");
        CHECK(cs.public_mutation_epoch() >= e1 + 1, "epoch advanced again on second");
    }

    // ── AC2: concurrent invalidate of distinct roots — no crash ──
    {
        CompilerService cs;
        // Shared graph: d0..d3 all call base
        for (int i = 0; i < 4; ++i)
            cs.public_record_dependency(std::format("d{}", i), "base");
        constexpr int kThreads = 4;
        constexpr int kIters = 100;
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(kThreads));
        const auto e0 = cs.public_mutation_epoch();
        const auto c0 = cs.public_invalidate_function_calls();
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                try {
                    for (int i = 0; i < kIters; ++i) {
                        // Alternate targets so cascades interleave
                        if ((i + t) % 2 == 0)
                            cs.public_invalidate_function("base");
                        else
                            cs.public_invalidate_function(std::format("d{}", t));
                        // Re-seed edges after erase so next BFS has work
                        cs.public_record_dependency(std::format("d{}", t), "base");
                    }
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "no exceptions under concurrent invalidate");
        const auto n = static_cast<std::uint64_t>(kThreads) * kIters;
        CHECK(cs.public_invalidate_function_calls() == c0 + n,
              std::format("calls advanced exactly N (got {} expected {})",
                          cs.public_invalidate_function_calls() - c0, n));
        // Epoch >= N: each entry bumps once under lock; shape/bridge
        // paths may add more. Exact equality is not required.
        CHECK(cs.public_mutation_epoch() >= e0 + n,
              std::format("epoch advanced >= N (got {} expected >= {})",
                          cs.public_mutation_epoch() - e0, n));
    }

    // ── AC3: concurrent record + invalidate — dep_graph stays consistent ──
    {
        CompilerService cs;
        constexpr int kWriters = 4;
        constexpr int kIters = 200;
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kWriters; ++t) {
            threads.emplace_back([&, t]() {
                try {
                    for (int i = 0; i < kIters; ++i) {
                        cs.public_record_dependency(std::format("w{}", t), "root");
                        cs.public_record_dependency(std::format("w{}", t), "leaf");
                        if (i % 5 == 0)
                            cs.public_invalidate_function("root");
                        if (i % 7 == 0)
                            cs.public_invalidate_function(std::format("w{}", t));
                    }
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "no exceptions under record+invalidate");
        // Post-stress: re-seed single edge, assert no duplicates
        cs.public_record_dependency("final", "root");
        cs.public_record_dependency("final", "root");
        CHECK(cs.public_dep_graph_calls_for("final") == 1, "final→root unique after stress");
        CHECK(cs.public_dep_graph_has_edge("final", "root"), "final→root present");
    }

    // ── AC4: many concurrent invalidates of same name — counters match ──
    {
        CompilerService cs;
        cs.public_record_dependency("a", "x");
        cs.public_record_dependency("b", "x");
        constexpr int kThreads = 8;
        constexpr int kIters = 50;
        std::atomic<int> errors{0};
        const auto e0 = cs.public_mutation_epoch();
        const auto c0 = cs.public_invalidate_function_calls();
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                try {
                    for (int i = 0; i < kIters; ++i)
                        cs.public_invalidate_function("x");
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "same-name concurrent invalidate no throw");
        const auto n = static_cast<std::uint64_t>(kThreads) * kIters;
        CHECK(cs.public_invalidate_function_calls() - c0 == n, "same-name: calls == N");
        CHECK(cs.public_mutation_epoch() - e0 >= n, "same-name: epoch >= N");
        // After mass invalidate, re-record should still be idempotent
        cs.public_record_dependency("a", "x");
        cs.public_record_dependency("a", "x");
        CHECK(cs.public_dep_graph_calls_for("a") == 1, "re-seed after mass invalidate unique");
    }

    // ── AC5: jit_cache presence helper is safe under concurrent invalidate ──
    {
        CompilerService cs;
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                try {
                    for (int i = 0; i < 100; ++i) {
                        cs.public_invalidate_function(std::format("fn{}", t));
                        (void)cs.public_jit_cache_contains(std::format("fn{}", t));
                        (void)cs.public_jit_cache_contains("missing");
                    }
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "jit_cache contains + invalidate concurrent OK");
        CHECK(!cs.public_jit_cache_contains("missing"), "missing name not in cache");
    }

    // ── AC6: sequential cascade still deterministic (#401 regression light) ──
    {
        CompilerService cs;
        cs.public_record_dependency("g", "f");
        cs.public_record_dependency("h", "g");
        CHECK(cs.public_dep_graph_has_edge("g", "f"), "g→f before invalidate");
        CHECK(cs.public_dep_graph_has_edge("h", "g"), "h→g before invalidate");
        cs.public_invalidate_function("f");
        // Dependents g,h cleaned from graph; f's own cleanup also runs
        CHECK(!cs.public_dep_graph_has_edge("g", "f") || !cs.public_dep_graph_contains("g"),
              "cascade cleaned g edge or entry");
        // Re-seed twice — still unique
        cs.public_record_dependency("g", "f");
        cs.public_record_dependency("g", "f");
        CHECK(cs.public_dep_graph_calls_for("g") == 1, "post-cascade reseed unique");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("invalidate cascade order #1378: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

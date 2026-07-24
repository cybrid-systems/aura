// test_arena_compact_hook_concurrent.cpp — Issue #1989: ASTArena::on_compact_hook_
// concurrent assign+invoke (A-003).
//
// Single-case stress test: N setter threads alternate set_on_compact_hook /
// take_on_compact_hook while M compacter threads call compact() (which invokes
// the hook). Without hook_mtx_ this is UB (concurrent std::function
// move-assign + invoke). With the mutex the run completes without crash and
// every observed hook invocation has a non-null callable.

#include "test_harness.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

import std;
import aura.core.arena;

namespace {
constexpr int kSetterThreads = 4;
constexpr int kCompacterThreads = 2;
constexpr int kItersPerThread = 50;
} // namespace

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    aura::ast::ASTArena arena;

    // Note: we don't pre-allocate — allocate_raw is private (post-#1546
    // quota gate). The mutex test only needs the hook set/take/invoke
    // path under concurrent compact(); compact() itself is safe on a
    // fresh arena.

    std::atomic<std::uint64_t> hook_calls{0};
    std::atomic<int> hook_errors{0};

    // Initial hook.
    arena.set_on_compact_hook(
        [&]() noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(arena.has_on_compact_hook(), "initial hook installed");

    // Setter threads: alternate set_on_compact_hook / take_on_compact_hook.
    std::vector<std::thread> setters;
    setters.reserve(kSetterThreads);
    for (int t = 0; t < kSetterThreads; ++t) {
        setters.emplace_back([&]() noexcept {
            for (int i = 0; i < kItersPerThread; ++i) {
                try {
                    arena.set_on_compact_hook(
                        [&]() noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
                } catch (...) {
                    ++hook_errors;
                }
                // Half the time, take the hook out (exercises the move path).
                if ((i & 1) == 0) {
                    (void)arena.take_on_compact_hook();
                }
            }
        });
    }

    // Compacter threads: invoke compact() repeatedly (fires the hook).
    std::vector<std::thread> compacters;
    compacters.reserve(kCompacterThreads);
    for (int t = 0; t < kCompacterThreads; ++t) {
        compacters.emplace_back([&]() noexcept {
            for (int i = 0; i < kItersPerThread; ++i) {
                try {
                    (void)arena.compact();
                } catch (...) {
                    ++hook_errors;
                }
            }
        });
    }

    for (auto& th : setters)
        th.join();
    for (auto& th : compacters)
        th.join();

    CHECK(hook_errors.load() == 0, "no exceptions under concurrent set/take + compact");
    CHECK(hook_calls.load() > 0, "hook fired at least once during compact");

    // Release allocations so arena destructor doesn't trip the per-slot dtor
    // walker on freed memory (we never installed per-slot dtors, so this is
    // a no-op — but be defensive).
    (void)arena.take_on_compact_hook();

    if (::aura::test::g_failed)
        return 1;
    std::println("arena compact-hook concurrent #1989: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

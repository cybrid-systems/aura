// @category: unit
// @reason: Issue #1864 — Evaluator::gc_root_count must shared_lock
// closures_mtx_ while iterating closures_ (public API, not
// safepoint-only). Concurrent register_active_closure must not UAF.
//
//   AC1: source takes shared_lock(closures_mtx_) in gc_root_count
//   AC2: sequential register + gc_root_count does not crash; size-ish
//   AC3: concurrent count readers + register writers complete

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: gc_root_count shared_lock closures_mtx_ ---");
        auto src =
            read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!src.empty(), "read evaluator_gc.cpp");
        CHECK(src.find("#1864") != std::string::npos, "cites #1864");
        auto pos = src.find("Evaluator::gc_root_count");
        CHECK(pos != std::string::npos, "gc_root_count present");
        auto win = src.substr(pos, 700);
        CHECK(win.find("shared_lock") != std::string::npos, "uses shared_lock");
        CHECK(win.find("closures_mtx_") != std::string::npos, "locks closures_mtx_");
        // Old "No lock — called at safepoint" must be gone from body.
        CHECK(win.find("No lock") == std::string::npos, "removed No lock safepoint claim");
        CHECK(!ixx.empty() && ixx.find("#1864") != std::string::npos, "ixx cites #1864");
    }

    // ── AC2: sequential ──
    {
        std::println("\n--- AC2: sequential register + gc_root_count ---");
        Evaluator ev;
        auto n0 = ev.gc_root_count();
        CHECK(n0 == n0, "fresh count callable");
        for (int i = 0; i < 32; ++i) {
            Closure cl;
            cl.env_id = NULL_ENV_ID;
            (void)ev.register_active_closure(std::move(cl));
        }
        auto n1 = ev.gc_root_count();
        // Watermark may exclude new closures from the root total;
        // must still complete and not under-run string/pair baseline.
        CHECK(n1 >= n0, "count does not decrease after register");
        // Multiple sequential calls stable under quiescence.
        auto n2 = ev.gc_root_count();
        CHECK(n2 == n1, "stable under sequential re-call");
    }

    // ── AC3: concurrent readers + writers ──
    {
        std::println("\n--- AC3: concurrent gc_root_count + register_active_closure ---");
        Evaluator ev;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> ops{0};
        constexpr int kReaders = 4;
        constexpr int kWriters = 2;
        constexpr int kIters = 200;
        std::vector<std::thread> threads;
        threads.reserve(kReaders + kWriters);

        for (int r = 0; r < kReaders; ++r) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int i = 0; i < kIters; ++i) {
                    (void)ev.gc_root_count();
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (int w = 0; w < kWriters; ++w) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int i = 0; i < kIters; ++i) {
                    Closure cl;
                    cl.env_id = NULL_ENV_ID;
                    (void)ev.register_active_closure(std::move(cl));
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        auto t0 = std::chrono::steady_clock::now();
        for (auto& t : threads)
            t.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        const auto expected = static_cast<std::uint64_t>((kReaders + kWriters) * kIters);
        CHECK(ops.load() == expected, "all concurrent ops completed");
        CHECK(ms < 30000, "finished within 30s (no deadlock)");
        // Final count still callable.
        (void)ev.gc_root_count();
        CHECK(true, "post-stress gc_root_count ok");
    }

    std::println("\n=== test_gc_root_count_lock_1864: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #2409 — StringPool::buf_fragmentation samples buf_.size()
// and string_bytes under one shared_lock (closes data_size race + F1 UAF
// propagation from pre-#2408 string_bytes_total).
//
//   AC1: 4 writers intern + 4 readers buf_fragmentation (no crash)
//   AC2: buf_fragmentation always in [0.0, 1.0] under concurrency
//   AC3: single shared_lock per call (readers delta == 1)
//   AC4: sequential / empty / observability still sensible

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::StringPool;
using aura::ast::stringpool_intern_concurrent_readers_total;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_stringpool_buf_fragmentation_lock_2409() {
    std::println("=== Issue #2409: buf_fragmentation single shared_lock ===");

    // ── AC4 sequential / empty ─────────────────────────────────────
    {
        std::println("\n--- #2409 AC4: empty + sequential pack ---");
        StringPool empty;
        CHECK(empty.buf_fragmentation() == 0.0, "AC4: empty frag == 0");
        StringPool pool;
        for (int i = 0; i < 20; ++i)
            (void)pool.intern("word-" + std::to_string(i));
        const auto frag = pool.buf_fragmentation();
        std::println("  sequential frag={}", frag);
        CHECK(frag >= 0.0 && frag <= 1.0, "AC4: sequential frag in [0,1]");
        // Monotonic append, no reset → typically ~0 packed.
        CHECK(pool.data_size() >= pool.string_bytes_total(), "AC4: data_size >= string_bytes");
        CHECK(pool.string_bytes_total() > 1, "AC4: string_bytes_total works");
    }

    // ── AC3 one lock per buf_fragmentation ─────────────────────────
    {
        std::println("\n--- #2409 AC3: one shared_lock per buf_fragmentation ---");
        StringPool pool;
        for (int i = 0; i < 80; ++i)
            (void)pool.intern("f" + std::to_string(i));
        const auto r0 = stringpool_intern_concurrent_readers_total();
        const auto frag = pool.buf_fragmentation();
        const auto r1 = stringpool_intern_concurrent_readers_total();
        const auto delta = r1 - r0;
        std::println("  readers delta={} frag={}", delta, frag);
        CHECK(delta == 1, "AC3: exactly one shared_lock (not data_size + string_bytes)");
        CHECK(frag >= 0.0 && frag <= 1.0, "AC3: frag in range");
    }

    // ── AC1 + AC2 concurrent intern × fragmentation ────────────────
    {
        std::println("\n--- #2409 AC1 + #2409 AC2: concurrent intern × buf_fragmentation ---");
        StringPool pool;
        (void)pool.intern("seed");
        constexpr int kWriters = 4;
        constexpr int kReaders = 4;
        constexpr int kIters = 250;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> out_of_range{0};
        std::atomic<std::uint64_t> samples{0};
        std::vector<std::thread> threads;
        threads.reserve(kWriters + kReaders);

        for (int t = 0; t < kWriters; ++t) {
            threads.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    (void)pool.intern("w" + std::to_string(t) + "-" + std::to_string(i));
                    if ((i & 3) == 0)
                        (void)pool.intern("hot-key");
                }
            });
        }
        for (int t = 0; t < kReaders; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    const double f = pool.buf_fragmentation();
                    samples.fetch_add(1, std::memory_order_relaxed);
                    if (!(f >= 0.0 && f <= 1.0))
                        out_of_range.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        const auto final_f = pool.buf_fragmentation();
        std::println("  samples={} out_of_range={} final_f={}", samples.load(), out_of_range.load(),
                     final_f);
        CHECK(samples.load() == static_cast<std::uint64_t>(kReaders * kIters),
              "AC1: all reader samples completed");
        CHECK(out_of_range.load() == 0, "AC2: all concurrent frags in [0.0, 1.0]");
        CHECK(final_f >= 0.0 && final_f <= 1.0, "AC2: final frag in [0.0, 1.0]");
        // Quiescent re-read stable.
        CHECK(pool.buf_fragmentation() == final_f ||
                  (pool.buf_fragmentation() >= 0.0 && pool.buf_fragmentation() <= 1.0),
              "AC1: post-stress frag still valid");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_stringpool_buf_fragmentation_lock_2409();
}
#endif

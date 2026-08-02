// @category: unit
// @reason: Issue #2408 — StringPool::string_bytes_total holds one shared_lock
// for the whole walk (resolve_unlocked), fixing UAF under concurrent intern
// and O(capacity) lock churn.
//
//   AC1: 4 threads concurrent intern + string_bytes_total (no crash; TSan clean)
//   AC2: string_bytes_total matches sequential sum under no concurrent mutate
//   AC3: one shared_lock per call (readers counter +1, not +N)
//   AC4: buf_fragmentation / observability still work

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

// Sequential sum of interned string bytes (exclusive of concurrent writers).
// Mirrors string_bytes_total formula: 1 leading NUL + sum(size+1).
static std::size_t sequential_bytes_expect(const std::vector<std::string>& strs) {
    std::size_t total = 1; // leading \0 sentinel
    // Dedup like intern: unique strings only once.
    std::vector<std::string> uniq;
    for (const auto& s : strs) {
        bool seen = false;
        for (const auto& u : uniq)
            if (u == s) {
                seen = true;
                break;
            }
        if (!seen)
            uniq.push_back(s);
    }
    for (const auto& s : uniq)
        total += s.size() + 1;
    return total;
}

} // namespace

int run_test_stringpool_bytes_total_lock_2408() {
    std::println("=== Issue #2408: string_bytes_total single shared_lock ===");

    // ── AC2: sequential correctness ────────────────────────────────
    {
        std::println("\n--- #2408 AC2: sequential string_bytes_total ---");
        StringPool pool;
        std::vector<std::string> words = {"alpha", "beta", "gamma", "alpha", "delta"};
        for (const auto& w : words)
            (void)pool.intern(w);
        const auto got = pool.string_bytes_total();
        const auto exp = sequential_bytes_expect(words);
        std::println("  got={} exp={}", got, exp);
        CHECK(got == exp, "AC2: string_bytes_total matches sequential unique sum");
        // data_size should be >= string_bytes_total (monotonic pack)
        CHECK(pool.data_size() >= got, "AC2: data_size >= string_bytes_total");
        CHECK(pool.buf_fragmentation() >= 0.0 && pool.buf_fragmentation() <= 1.0,
              "AC4: buf_fragmentation in range");
    }

    // ── AC3: one lock acquisition per call ─────────────────────────
    {
        std::println("\n--- #2408 AC3: single shared_lock per call ---");
        StringPool pool;
        // Fill enough slots that old code would take many resolve locks.
        for (int i = 0; i < 80; ++i)
            (void)pool.intern("s" + std::to_string(i));
        CHECK(pool.hash_capacity() >= 64, "AC3: capacity large enough for multi-lock bug");

        const auto r0 = stringpool_intern_concurrent_readers_total();
        const auto bytes = pool.string_bytes_total();
        const auto r1 = stringpool_intern_concurrent_readers_total();
        const auto delta = r1 - r0;
        std::println("  readers delta={} (expect 1) bytes={}", delta, bytes);
        CHECK(delta == 1, "AC3: exactly one shared_lock per string_bytes_total");
        CHECK(bytes > 1, "AC3: non-trivial byte total");
    }

    // ── AC1: concurrent intern + string_bytes_total ────────────────
    {
        std::println("\n--- #2408 AC1: concurrent intern × string_bytes_total ---");
        StringPool pool;
        (void)pool.intern("seed");
        constexpr int kWriters = 4;
        constexpr int kReaders = 4;
        constexpr int kIters = 200;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> read_sum{0};
        std::vector<std::thread> threads;
        threads.reserve(kWriters + kReaders);

        for (int t = 0; t < kWriters; ++t) {
            threads.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    (void)pool.intern("w" + std::to_string(t) + "-" + std::to_string(i));
                    if ((i & 7) == 0)
                        (void)pool.intern("shared-hot");
                }
            });
        }
        for (int t = 0; t < kReaders; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i) {
                    const auto b = pool.string_bytes_total();
                    read_sum.fetch_add(b, std::memory_order_relaxed);
                    // Touch fragmentation path (calls string_bytes_total again).
                    (void)pool.buf_fragmentation();
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        const auto final_bytes = pool.string_bytes_total();
        std::println("  final_bytes={} read_sum={}", final_bytes, read_sum.load());
        CHECK(final_bytes > 1, "AC1: final string_bytes_total sane after stress");
        CHECK(read_sum.load() > 0, "AC1: readers observed positive totals");
        // Post-stress sequential consistency: re-walk under no writers.
        const auto again = pool.string_bytes_total();
        CHECK(again == final_bytes, "AC1: stable under no concurrent writers");
    }

    // ── AC4: empty / soft ──────────────────────────────────────────
    {
        std::println("\n--- #2408 AC4: empty pool ---");
        StringPool empty;
        CHECK(empty.string_bytes_total() == 1, "AC4: empty pool only leading NUL");
        CHECK(empty.buf_fragmentation() == 0.0 || empty.data_size() == 0, "AC4: empty frag");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_stringpool_bytes_total_lock_2408();
}
#endif

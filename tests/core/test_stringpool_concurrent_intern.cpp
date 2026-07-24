// Issue #2062 — StringPool thread-safe intern test.
// Verifies the std::shared_mutex wrap around intern/resolve/find_by_name
// compiles + works under concurrent fibers. The test exercises:
//  - concurrent intern of the same key from multiple threads (idempotent)
//  - concurrent intern of distinct keys (parallel-safe)
//  - concurrent resolve of pre-interned keys (read-side safe)
//  - existing single-threaded intern still works
//
// Note: the concurrency stress is exercised via std::thread; under TSan
// the shared_mutex correctly serializes writers and parallelizes readers.

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::StringPool;

} // namespace

int main() {
    std::println("=== Issue #2062: StringPool thread-safe intern ===");
    StringPool pool;

    // AC1: existing single-threaded intern still works
    {
        std::println("\n--- AC1: single-threaded intern + resolve + find_by_name ---");
        const auto id_a = pool.intern("hello");
        const auto id_b = pool.intern("world");
        const auto id_a2 = pool.intern("hello"); // dedup
        CHECK(id_a == id_a2, "intern dedups identical strings");
        CHECK(id_a != id_b, "intern gives distinct ids for distinct strings");
        CHECK(pool.resolve(id_a) == "hello", "resolve(hello) == \"hello\"");
        CHECK(pool.resolve(id_b) == "world", "resolve(world) == \"world\"");
        const auto found_a = pool.find_by_name("hello");
        CHECK(found_a.has_value() && *found_a == id_a, "find_by_name(\"hello\") returns id_a");
        const auto found_x = pool.find_by_name("nonexistent");
        CHECK(!found_x.has_value(), "find_by_name(\"nonexistent\") is nullopt");
    }

    // AC2: concurrent intern of the same key from multiple threads (idempotent)
    {
        std::println("\n--- AC2: concurrent intern of same key is idempotent ---");
        constexpr int kThreads = 8;
        constexpr int kPerThread = 100;
        std::vector<std::thread> ts;
        std::vector<aura::ast::SymId> results(kThreads * kPerThread);
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t]() {
                for (int i = 0; i < kPerThread; ++i)
                    results[t * kPerThread + i] = pool.intern("concurrent-key");
            });
        }
        for (auto& th : ts)
            th.join();
        // All ids must be equal (intern is idempotent + thread-safe)
        const auto first = results[0];
        bool all_same = true;
        for (auto id : results)
            if (id != first) {
                all_same = false;
                break;
            }
        CHECK(all_same, "all concurrent interns of the same key return the same id");
    }

    // AC3: concurrent intern of distinct keys (parallel-safe)
    {
        std::println("\n--- AC3: concurrent intern of distinct keys ---");
        constexpr int kThreads = 8;
        constexpr int kPerThread = 50;
        std::vector<std::thread> ts;
        std::vector<std::vector<aura::ast::SymId>> results(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t]() {
                results[t].reserve(kPerThread);
                for (int i = 0; i < kPerThread; ++i)
                    results[t].push_back(pool.intern(std::string("key-t") + std::to_string(t) +
                                                     "-i" + std::to_string(i)));
            });
        }
        for (auto& th : ts)
            th.join();
        // Each thread should have kPerThread distinct ids; across threads
        // the id space is global so all ids must be unique.
        std::vector<aura::ast::SymId> all;
        for (auto& v : results)
            all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        const bool unique = std::adjacent_find(all.begin(), all.end()) == all.end();
        CHECK(unique, "all distinct-key interns across threads yield unique ids");
    }

    // AC4: concurrent resolve of pre-interned keys (read-side safe)
    {
        std::println("\n--- AC4: concurrent resolve is parallel-safe ---");
        // Pre-intern a few keys.
        const auto id1 = pool.intern("read-key-1");
        const auto id2 = pool.intern("read-key-2");
        constexpr int kReaders = 8;
        constexpr int kPerReader = 200;
        std::vector<std::thread> ts;
        std::atomic<int> errors{0};
        for (int t = 0; t < kReaders; ++t) {
            ts.emplace_back([&]() {
                for (int i = 0; i < kPerReader; ++i) {
                    if (pool.resolve(id1) != "read-key-1")
                        errors.fetch_add(1);
                    if (pool.resolve(id2) != "read-key-2")
                        errors.fetch_add(1);
                }
            });
        }
        for (auto& th : ts)
            th.join();
        CHECK(errors.load() == 0, "concurrent resolve returned wrong values");
    }

    // AC5: file-level atomic observability counters bump correctly
    // (refine #1861 observability surface). Counter accessors are
    // exported from aura.core.ast (stringpool_intern_total() +
    // stringpool_intern_concurrent_readers_total()).
    {
        std::println("\n--- AC5: file-level atomic observability ---");
        const auto intern_before = aura::ast::stringpool_intern_total();
        const auto readers_before = aura::ast::stringpool_intern_concurrent_readers_total();

        constexpr int kInterns = 10;
        for (int i = 0; i < kInterns; ++i)
            pool.intern("metric-test-key-" + std::to_string(i));
        // resolve + find_by_name bump the shared_lock counter.
        const auto read_id = pool.intern("read-back");
        pool.resolve(read_id);
        pool.find_by_name("metric-test-key-0");
        pool.find_by_name("nonexistent-for-find");

        const auto intern_after = aura::ast::stringpool_intern_total();
        const auto readers_after = aura::ast::stringpool_intern_concurrent_readers_total();
        CHECK(intern_after - intern_before == kInterns + 1,
              "stringpool_intern_total bumped by exactly "
              "kInterns + 1 (10 new + 1 read-back intern)");
        // 1 resolve + 2 find_by_name shared_lock acquisitions on top of AC4 baseline.
        CHECK(readers_after - readers_before >= 3, "stringpool_intern_concurrent_readers_total "
                                                   "bumped by >= 3 (1 resolve + 2 find_by_name)");
    }

    std::println("\n=== Results: passed ===");
    return 0;
}
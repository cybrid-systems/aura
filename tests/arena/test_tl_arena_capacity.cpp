// test_tl_arena_capacity.cpp — Issue #1359: TLarena 1MB default + graceful OOM

#include "test_harness.hpp"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

int main() {
    // AC1: default capacity constant is 1MB (not 64MB)
    CHECK(TLarena::kDefaultCapacity == 1024u * 1024u, "kDefaultCapacity == 1MB");
    CHECK(aura_tl_arena_default_capacity() == TLarena::kDefaultCapacity,
          "aura_tl_arena_default_capacity probe");
    CHECK(TLarena::kDefaultCapacity < 64u * 1024u * 1024u, "default < former 64MB");

    // Fresh TLarena: capacity 0 until resolve
    {
        TLarena a{};
        CHECK(a.base == nullptr, "fresh base null");
        CHECK(a.capacity == 0, "fresh capacity 0 (lazy resolve)");
        CHECK(a.offset == 0, "fresh offset 0");
        CHECK(tl_arena_init(&a), "init succeeds");
        CHECK(a.base != nullptr, "init allocates base");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "init capacity == 1MB");
        CHECK(a.offset == 0, "init offset 0");
        tl_arena_destroy(&a);
        CHECK(a.base == nullptr, "destroy clears base");
    }

    // Small alloc stays within 1MB (no double)
    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init for small alloc");
        void* p = tl_arena_alloc(&a, 64, 8);
        CHECK(p != nullptr, "small alloc non-null");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "small alloc keeps 1MB");
        CHECK(a.offset >= 64, "offset advanced");
        std::memset(p, 0xAB, 64);
        tl_arena_destroy(&a);
    }

    // Lazy init via alloc (fiber path: no explicit tl_arena_init)
    {
        TLarena a{};
        void* p = tl_arena_alloc(&a, 32, 8);
        CHECK(p != nullptr, "lazy alloc non-null");
        CHECK(a.base != nullptr, "lazy init set base");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "lazy init capacity 1MB");
        tl_arena_destroy(&a);
    }

    // AC3: doubling preserved — force growth past 1MB
    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init for grow");
        const size_t big = TLarena::kDefaultCapacity + 4096;
        void* p = tl_arena_alloc(&a, big, 8);
        CHECK(p != nullptr, "grow alloc non-null");
        CHECK(a.capacity >= big, "capacity >= request");
        CHECK(a.capacity >= TLarena::kDefaultCapacity * 2, "at least one double");
        // Second large alloc may double again
        size_t cap_before = a.capacity;
        void* q = tl_arena_alloc(&a, a.capacity, 8);
        CHECK(q != nullptr, "second grow non-null");
        CHECK(a.capacity >= cap_before, "capacity non-decreasing");
        tl_arena_destroy(&a);
    }

    // Explicit capacity preserved when non-zero
    {
        TLarena a{};
        a.capacity = 4096;
        CHECK(tl_arena_init(&a), "init with explicit 4K");
        CHECK(a.capacity == 4096, "explicit capacity kept");
        void* p = tl_arena_alloc(&a, 64, 8);
        CHECK(p != nullptr, "alloc in 4K arena");
        // Exceed → double to 8K
        void* q = tl_arena_alloc(&a, 5000, 8);
        CHECK(q != nullptr, "alloc forcing double from 4K");
        CHECK(a.capacity >= 8192, "doubled from 4K");
        tl_arena_destroy(&a);
    }

    // push/pop mark (no intervening alloc — stack sits at current offset)
    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init push/pop");
        void* p = tl_arena_alloc(&a, 16, 8);
        CHECK(p != nullptr, "alloc before push");
        size_t off = a.offset;
        tl_arena_push(&a);
        CHECK(a.offset > off, "push advanced offset");
        tl_arena_pop(&a);
        CHECK(a.offset == off, "pop restored offset");
        tl_arena_destroy(&a);
    }

    // AC2: OOM path does not exit — simulate via absurd capacity request
    {
        const auto oom_before = aura_tl_arena_oom_total();
        TLarena a{};
        // Request more than any reasonable malloc (may succeed under overcommit).
        // When it fails: base stays null, no process exit.
        a.capacity = static_cast<size_t>(1) << 62;
        bool ok = tl_arena_init(&a);
        if (!ok) {
            CHECK(a.base == nullptr, "OOM init leaves base null");
            CHECK(aura_tl_arena_oom_total() > oom_before, "OOM counter incremented");
            // alloc after failed init also returns null without exit
            void* p = tl_arena_alloc(&a, 16, 8);
            // resolve may retry with same huge capacity → still null
            CHECK(p == nullptr || a.base != nullptr, "alloc either fails or recovers");
            if (a.base)
                tl_arena_destroy(&a);
        } else {
            // Overcommit: init "succeeded" — still valid, just free it
            CHECK(a.base != nullptr, "overcommit path has base");
            tl_arena_destroy(&a);
            CHECK(true, "OOM path skipped (kernel overcommit)");
        }
    }

    // AC4 proxy: 100 threads × small alloc → each capacity stays 1MB
    // (logical bound: 100 * 1MB = 100MB << 6.4GB / 200MB budget)
    {
        constexpr int kThreads = 100;
        std::atomic<int> ok_count{0};
        std::atomic<size_t> total_cap{0};
        std::atomic<size_t> max_cap{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&] {
                TLarena local{};
                if (!tl_arena_init(&local))
                    return;
                void* p = tl_arena_alloc(&local, 128, 8);
                if (!p) {
                    tl_arena_destroy(&local);
                    return;
                }
                total_cap.fetch_add(local.capacity, std::memory_order_relaxed);
                size_t prev = max_cap.load(std::memory_order_relaxed);
                while (local.capacity > prev &&
                       !max_cap.compare_exchange_weak(prev, local.capacity,
                                                      std::memory_order_relaxed)) {
                }
                if (local.capacity == TLarena::kDefaultCapacity)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                tl_arena_destroy(&local);
            });
        }
        for (auto& t : threads)
            t.join();

        CHECK(ok_count.load() == kThreads, "all 100 threads at 1MB capacity");
        CHECK(max_cap.load() == TLarena::kDefaultCapacity, "no thread exceeded 1MB");
        CHECK(total_cap.load() == static_cast<size_t>(kThreads) * TLarena::kDefaultCapacity,
              "total logical capacity == 100MB");
        CHECK(total_cap.load() < 200u * 1024u * 1024u, "total < 200MB budget");
    }

    // Null arena guards
    CHECK(!tl_arena_init(nullptr), "init nullptr fails");
    CHECK(tl_arena_alloc(nullptr, 8, 8) == nullptr, "alloc nullptr fails");
    tl_arena_destroy(nullptr);
    tl_arena_reset(nullptr);
    tl_arena_push(nullptr);
    tl_arena_pop(nullptr);
    CHECK(true, "null guards no crash");

    if (::aura::test::g_failed)
        return 1;
    std::println("tl_arena capacity #1359: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

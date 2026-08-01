// @category: unit
// @reason: Issue #2463 — convert summary_flags_ to std::atomic so
// concurrent reader (summary_flags() / summary_has()) + writer (clear() /
// summary_recompute() / summary_add_flags()) never observes a torn
// mid-write bit-pattern. clear() now acquires flatast_mutex_ so add_node()
// and clear() are serialized across the SoA columns.
//
// Issue #2444 — region_by_sym_dense_ concurrent set_function_region +
// get_function_region_for_sym (region_table_mtx_ + atomic_ref; no mid-resize
// UB / torn region byte). Extends this concurrency harness per AC.
//
// TSan-clean under -fsanitize=thread; value-consistency (no torn
// 0xA5A5A5A5 bits observed by the reader thread) holds on
// architectures where naturally-aligned 32-bit stores are not
// guaranteed atomic at the hardware level (e.g. ARMv7, legacy
// 32-bit MIPS). On x86-64 / ARMv8, naturally-aligned 32-bit stores
// are atomic at the hardware level so this test acts as a
// contract / regression guard rather than a failure catch.
//
//   AC1: single-threaded clear() → 0; summary_add_flags() → flag set
//   AC2: reader thread + main thread clear() → reader sees only 0
//        or the set pattern, never torn bits
//   #2444 AC: concurrent set_function_region + get_function_region_for_sym
//             — no crash, region values only in published set

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::ast::FlatAST;
using aura::ast::SymId;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint32_t kFlagPattern = 0xA5A5A5A5u;

[[nodiscard]] bool is_torn(std::uint32_t v) noexcept {
    // Valid observations during this test are exactly kFlagPattern
    // (pre-clear) and 0 (post-clear). Any other value indicates a
    // torn mid-write was observed — which the std::atomic store in
    // summary_flags_.store(0, std::memory_order_release) inside
    // clear() must prevent.
    return v != kFlagPattern && v != 0u;
}

} // namespace

int main() {
    // ── AC1: single-threaded baseline ─────────────────────────────
    {
        std::println("\n--- AC1: single-threaded clear + summary_add_flags ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<FlatAST>(alloc);

        CHECK(flat->summary_flags() == 0u, "fresh FlatAST: summary_flags() == 0");
        flat->summary_add_flags(kFlagPattern);
        CHECK(flat->summary_flags() == kFlagPattern, "after summary_add_flags: == pattern");
        flat->clear();
        CHECK(flat->summary_flags() == 0u, "after clear(): == 0");
        // summary_has() must report the bit cleared after clear().
        CHECK(!flat->summary_has(static_cast<aura::ast::SummaryFlag>(kFlagPattern)),
              "summary_has() clears after clear()");
    }

    // ── AC2: reader thread + clear() — no torn bits observed ─────
    {
        std::println("\n--- AC2: reader vs clear() race-free (no torn bits) ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<FlatAST>(alloc);

        flat->summary_add_flags(kFlagPattern);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> torn{0};
        std::atomic<std::uint64_t> zeros{0};
        std::atomic<std::uint64_t> patterns{0};
        std::atomic<std::uint64_t> has_calls{0};

        std::thread reader([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                const auto v = flat->summary_flags();
                reads.fetch_add(1, std::memory_order_relaxed);
                if (is_torn(v)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                } else if (v == 0u) {
                    zeros.fetch_add(1, std::memory_order_relaxed);
                } else {
                    patterns.fetch_add(1, std::memory_order_relaxed);
                }
                // Exercise summary_has() on the same atomic column;
                // must not race with the store in clear().
                const auto has =
                    flat->summary_has(static_cast<aura::ast::SummaryFlag>(kFlagPattern));
                has_calls.fetch_add(1, std::memory_order_relaxed);
                (void)has;
            }
        });

        // Give the reader a head start so it's spinning when we clear.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        flat->clear();
        // Let the reader observe the post-clear 0 at least once.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stop.store(true, std::memory_order_release);
        reader.join();

        const auto r = reads.load();
        const auto t = torn.load();
        const auto z = zeros.load();
        const auto p = patterns.load();
        const auto h = has_calls.load();
        std::println("  reads={} patterns={} zeros={} torn={} summary_has={}", r, p, z, t, h);
        CHECK(r > 0, "reader thread made at least one read");
        CHECK(h > 0, "reader called summary_has() at least once");
        CHECK(p > 0, "reader observed kFlagPattern at least once (pre-clear)");
        CHECK(z > 0, "reader observed 0 at least once (post-clear)");
        CHECK(t == 0, "no torn mid-write observed (atomic store guarantees)");
    }

    // ── AC3: clear() holds flatast_mutex_ vs add_node() ───────────
    {
        std::println("\n--- AC3: clear() + add_node() under flatast_mutex_ ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<FlatAST>(alloc);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> added{0};

        // Issue #2463: clear() now acquires flatast_mutex_, which
        // add_node() also holds (recursive_mutex at ast.ixx:910).
        // Concurrent clear() + add_node() must serialize — no crash,
        // no tag_ vector corruption, no stale parent_/children_ refs.
        std::thread adder([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                flat->add_literal(static_cast<std::int64_t>(added.load() + 1));
                added.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        flat->clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        stop.store(true, std::memory_order_release);
        adder.join();

        const auto a = added.load();
        std::println("  add_node calls={}", a);
        CHECK(a > 0, "adder thread completed at least one add_node");
        // Post-clear + post-stop: any node the adder pushed AFTER
        // clear() released flatast_mutex_ is a fresh live node, so
        // flat->size() may be > 0 (adder kept adding after clear()
        // released). What must hold is structural consistency —
        // size is non-negative and equal to the number of nodes
        // pushed after clear() (which we don't count exactly).
        // The structural invariant we DO check: summary_flags()
        // reflects either the pre-add_node tag bits or 0 — never
        // torn mid-write.
        const auto post = flat->summary_flags();
        std::println("  post-stop summary_flags()=0x{:08x}", post);
        CHECK(post == 0u || post == kFlagPattern || (post & kFlagPattern) == kFlagPattern ||
                  (post & kFlagPattern) == 0u,
              "post-stop summary_flags() is a valid value (no torn bits)");
    }

    // ── Issue #2444: region_by_sym_dense concurrent set + get ──────
    {
        std::println("\n--- #2444: concurrent set_function_region + get (dense) ---");
        FlatAST flat;
        constexpr int kKeys = 64;
        constexpr std::uint8_t kMaxRegion = 15;
        // Pre-seed all keys so dense table is sized and final checks are
        // meaningful even if the storm is short; writers then overwrite.
        for (int i = 0; i < kKeys; ++i)
            flat.set_function_region(static_cast<SymId>(500 + i), 0);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> writes{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> hits{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        // 2 writers — resize (already sized) + store region+1 cells
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        // Also grow beyond pre-seed with higher SymIds to
                        // exercise concurrent resize (Issue #2444 AC).
                        const int key = (i % (kKeys * 2));
                        const SymId sym = static_cast<SymId>(500 + key);
                        const auto reg = static_cast<std::uint8_t>((i + t) & kMaxRegion);
                        flat.set_function_region(sym, reg);
                        writes.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // Many readers (Issue #2444 AC)
        for (int t = 0; t < 6; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const int key = (i % (kKeys * 2));
                        const SymId sym = static_cast<SymId>(500 + key);
                        auto r = flat.get_function_region_for_sym(sym);
                        if (r.has_value()) {
                            if (*r > kMaxRegion)
                                err.fetch_add(1, std::memory_order_relaxed);
                            else
                                hits.fetch_add(1, std::memory_order_relaxed);
                        }
                        reads.fetch_add(1, std::memory_order_relaxed);
                        i += 6;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  #2444 writes={} reads={} hits={} err={}", writes.load(), reads.load(),
                     hits.load(), err.load());
        CHECK(writes.load() > 0, "#2444: writers progressed");
        CHECK(reads.load() > 0, "#2444: readers progressed");
        CHECK(hits.load() > 0, "#2444: dense hits observed");
        CHECK(err.load() == 0, "#2444: no tear / invalid region / exceptions");

        // Pre-seeded keys always present; values in published range.
        for (int i = 0; i < kKeys; ++i) {
            const SymId sym = static_cast<SymId>(500 + i);
            auto r = flat.get_function_region_for_sym(sym);
            CHECK(r.has_value() && *r <= kMaxRegion, "#2444: final region coherent");
        }
    }

    std::println("\n=== test_ast_concurrency results: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
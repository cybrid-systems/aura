// @category: unit
// @reason: Issue #2417 — binding_gens_ atomic shared_ptr + COW bump.
//
//   AC1: atomic shared_ptr snapshot for readers
//   AC2: concurrent bump + binding_gen lookup (no crash / consistent)
//   AC3: compact/clone fresh map doesn't corrupt parent snapshot
//   AC4: sequential bump/read type-cache gen semantics preserved

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::ast::SymId;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_binding_gens_atomic_2417() {
    std::println("=== Issue #2417: binding_gens_ atomic shared_ptr ===");

    // ── AC1 sequential bump + read (atomic snapshot path) ─────────
    {
        std::println("\n--- #2417 AC1: sequential bump/read ---");
        StringPool pool;
        FlatAST flat;
        const SymId a = pool.intern("alpha");
        CHECK(flat.binding_gen(a) == 0, "AC1: default gen 0");
        flat.bump_binding_gen(a);
        CHECK(flat.binding_gen(a) == 1, "AC1: after one bump");
    }

    // ── AC4 multi-binding gen semantics preserved ──────────────────
    {
        std::println("\n--- #2417 AC4: multi-binding gen semantics ---");
        StringPool pool;
        FlatAST flat;
        const SymId a = pool.intern("alpha");
        const SymId b = pool.intern("beta");
        flat.bump_binding_gen(a);
        flat.bump_binding_gen(a);
        flat.bump_binding_gen(b);
        CHECK(flat.binding_gen(a) == 2, "AC4: a==2");
        CHECK(flat.binding_gen(b) == 1, "AC4: b==1");
        CHECK(flat.binding_gen_bumps_total() >= 3, "AC4: bump total");
    }

    // ── AC2 concurrent readers + bumpers ───────────────────────────
    {
        std::println("\n--- #2417 AC2: concurrent bump + lookup ---");
        StringPool pool;
        FlatAST flat;
        const SymId s = pool.intern("hot-binding");
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> bumps{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> nonmono{0};
        std::atomic<std::uint32_t> last_seen{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                if (t % 2 == 0) {
                    while (!stop.load(std::memory_order_acquire)) {
                        flat.bump_binding_gen(s);
                        bumps.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    std::uint32_t prev = 0;
                    while (!stop.load(std::memory_order_acquire)) {
                        const auto g = flat.binding_gen(s);
                        reads.fetch_add(1, std::memory_order_relaxed);
                        // Gen is monotonically non-decreasing for a single
                        // reader thread (snapshots may lag but never go back
                        // within one observation after a higher value was
                        // published to this thread's last_seen atomic).
                        last_seen.store(g, std::memory_order_relaxed);
                        if (g < prev)
                            nonmono.fetch_add(1, std::memory_order_relaxed);
                        prev = g;
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  bumps={} reads={} nonmono={} final={}", bumps.load(), reads.load(),
                     nonmono.load(), flat.binding_gen(s));
        CHECK(bumps.load() > 0, "AC2: bumps progressed");
        CHECK(reads.load() > 0, "AC2: reads progressed");
        CHECK(nonmono.load() == 0, "AC2: per-reader gen non-decreasing");
        CHECK(flat.binding_gen(s) == bumps.load(), "AC2: final gen == bump count");
    }

    // ── AC3 clone/copy isolation ───────────────────────────────────
    {
        std::println("\n--- #2417 AC3: copy share then independent COW bump ---");
        StringPool pool;
        FlatAST parent;
        const SymId s = pool.intern("shared");
        parent.bump_binding_gen(s);
        parent.bump_binding_gen(s);
        CHECK(parent.binding_gen(s) == 2, "AC3: parent gen 2");

        FlatAST child = parent; // shares snapshot initially
        CHECK(child.binding_gen(s) == 2, "AC3: child sees parent gen");

        child.bump_binding_gen(s); // COW — new map
        CHECK(child.binding_gen(s) == 3, "AC3: child gen 3 after bump");
        CHECK(parent.binding_gen(s) == 2, "AC3: parent still 2 (COW isolation)");

        parent.bump_binding_gen(s);
        CHECK(parent.binding_gen(s) == 3, "AC3: parent 3 after own bump");
        CHECK(child.binding_gen(s) == 3, "AC3: child unchanged by parent bump");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_binding_gens_atomic_2417();
}
#endif

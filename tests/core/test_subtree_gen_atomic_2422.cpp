// @category: unit
// @reason: Issue #2422 — subtree_gen_ cells are atomic (uint32 + atomic_ref).
//
//   AC1: element reads are atomic (no torn uint16; 32-bit cells)
//   AC2: concurrent bump_generation_subtree + is_valid_subtree (TSan-friendly)
//   AC3: #392 subtree semantics preserved (scoped invalidation)
//   AC4: std::atomic<uint32_t>::is_always_lock_free (compile-time + runtime)

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
using aura::ast::NodeId;
using aura::ast::StringPool;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_subtree_gen_atomic_2422() {
    std::println("=== Issue #2422: subtree_gen_ atomic cells ===");

    // ── AC4 is_always_lock_free ────────────────────────────────────
    {
        std::println("\n--- #2422 AC4: uint32 atomic is_always_lock_free ---");
        CHECK(std::atomic<std::uint32_t>::is_always_lock_free,
              "AC4: std::atomic<uint32_t>::is_always_lock_free");
        CHECK(std::atomic_ref<std::uint32_t>::is_always_lock_free,
              "AC4: std::atomic_ref<uint32_t>::is_always_lock_free");
    }

    // ── AC1/AC3: #392 scoped bump semantics with atomic cells ──────
    {
        std::println("\n--- #2422 AC1 + #2422 AC3: atomic load + #392 scoped invalidation ---");
        FlatAST flat;
        StringPool pool;
        const auto lit_a = flat.add_literal(1);
        const auto def_a = flat.add_define(pool.intern("a2422"), lit_a);
        const auto lit_b = flat.add_literal(2);
        const auto def_b = flat.add_define(pool.intern("b2422"), lit_b);

        const auto g0_a = flat.subtree_generation(def_a);
        const auto g0_b = flat.subtree_generation(def_b);
        CHECK(g0_a == 0, "AC1: initial subtree gen A is 0");
        CHECK(g0_b == 0, "AC1: initial subtree gen B is 0");

        const auto ref_a = flat.make_ref(lit_a);
        const auto ref_b = flat.make_ref(lit_b);
        CHECK(flat.is_valid_subtree(ref_a), "AC3: ref_a valid before bump");
        CHECK(flat.is_valid_subtree(ref_b), "AC3: ref_b valid before bump");

        flat.bump_generation_subtree(def_a);
        const auto g1_a = flat.subtree_generation(def_a);
        const auto g1_b = flat.subtree_generation(def_b);
        std::println("  after bump A: gen_a={} gen_b={}", g1_a, g1_b);
        CHECK(g1_a > g0_a, "AC1: atomic load sees bumped gen A");
        CHECK(g1_b == g0_b, "AC3: sibling subtree gen B unchanged");

        CHECK(!flat.is_valid_subtree(ref_a), "AC3: ref_a invalidated by scoped bump");
        CHECK(flat.is_valid_subtree(ref_b), "AC3: ref_b still valid (untouched subtree)");

        // Fresh capture after bump is valid again (restamp + new capture).
        const auto ref_a2 = flat.make_ref(lit_a);
        CHECK(flat.is_valid_subtree(ref_a2), "AC3: fresh ref after bump is valid");
        CHECK(ref_a2.subtree_gen_at_capture == g1_a, "AC1: capture matches atomic load");
    }

    // ── AC2: concurrent bump + is_valid_subtree ────────────────────
    {
        std::println("\n--- #2422 AC2: concurrent bump + is_valid_subtree ---");
        FlatAST flat;
        StringPool pool;
        constexpr int kDefs = 8;
        std::vector<NodeId> defs;
        std::vector<NodeId> lits;
        defs.reserve(kDefs);
        lits.reserve(kDefs);
        for (int i = 0; i < kDefs; ++i) {
            const auto lit = flat.add_literal(i + 1);
            lits.push_back(lit);
            defs.push_back(flat.add_define(pool.intern(std::format("d{}", i)), lit));
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> bump_ops{0};
        std::atomic<std::uint64_t> valid_ops{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        // 2 bumpers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        flat.bump_generation_subtree(defs[static_cast<std::size_t>(i % kDefs)]);
                        bump_ops.fetch_add(1, std::memory_order_relaxed);
                        i += 2;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 4 validators (make_ref + is_valid_subtree + subtree_generation)
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto idx = static_cast<std::size_t>(i % kDefs);
                        const auto ref = flat.make_ref(lits[idx]);
                        (void)flat.is_valid_subtree(ref);
                        (void)flat.subtree_generation(defs[idx]);
                        valid_ops.fetch_add(1, std::memory_order_relaxed);
                        i += 4;
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

        std::println("  bump_ops={} valid_ops={} err={}", bump_ops.load(), valid_ops.load(),
                     err.load());
        CHECK(bump_ops.load() > 0, "AC2: concurrent bumps progressed");
        CHECK(valid_ops.load() > 0, "AC2: concurrent validates progressed");
        CHECK(err.load() == 0, "AC2: no exceptions under concurrency");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_subtree_gen_atomic_2422();
}
#endif

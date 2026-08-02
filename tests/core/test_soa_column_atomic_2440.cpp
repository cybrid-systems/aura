// @category: unit
// @reason: Issue #2440 — 4 SoA side-table columns concurrent-safe via
//          dirty_column_mtx_ + std::atomic_ref (no torn reads).
//
//   AC1: concurrent reader + writer does not tear (epoch / stale / dirty)
//   AC2: 4 threads concurrent write+read per column (TSan-friendly)
//   AC3: semantics preserved (verify_dirty, verification_dirty,
//        last_seen_epoch, is_occurrence_stale)
//   AC4: std::atomic<uint8_t/uint64_t>::is_always_lock_free

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

int run_test_soa_column_atomic_2440() {
    std::println("=== Issue #2440: SoA column atomic_ref (no torn reads) ===");

    // ── AC4 is_always_lock_free ────────────────────────────────────
    {
        std::println("\n--- #2440 AC4: uint8/uint64 atomic is_always_lock_free ---");
        CHECK(std::atomic<std::uint8_t>::is_always_lock_free,
              "AC4: std::atomic<uint8_t>::is_always_lock_free");
        CHECK(std::atomic<std::uint64_t>::is_always_lock_free,
              "AC4: std::atomic<uint64_t>::is_always_lock_free");
        CHECK(std::atomic_ref<std::uint8_t>::is_always_lock_free,
              "AC4: std::atomic_ref<uint8_t>::is_always_lock_free");
        CHECK(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "AC4: std::atomic_ref<uint64_t>::is_always_lock_free");
    }

    // ── AC3: single-thread semantics preserved ─────────────────────
    {
        std::println("\n--- #2440 AC3: single-thread semantics ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);

        CHECK(flat.verify_dirty(id) == 0, "AC3: verify clean");
        CHECK(flat.verification_dirty(id) == 0, "AC3: verification clean");
        CHECK(flat.last_seen_epoch(id) == 0, "AC3: epoch 0");
        CHECK(flat.is_occurrence_stale(id) == 0, "AC3: occ fresh");

        flat.apply_verify_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kSvaDirty));
        CHECK((flat.verify_dirty(id) & FlatAST::kSvaDirty) != 0, "AC3: sva bit set");

        flat.apply_verification_dirty_bits(
            id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty));
        CHECK((flat.verification_dirty(id) & FlatAST::kCoverageFeedbackDirty) != 0,
              "AC3: coverage feedback set");
        CHECK(flat.is_verification_dirty(id), "AC3: is_verification_dirty");
        CHECK(flat.is_verification_dirty_for(id, FlatAST::kCoverageFeedbackDirty),
              "AC3: is_verification_dirty_for");

        flat.stamp_last_seen_epoch(id, 0xDEADBEEFCAFEBABEull);
        CHECK(flat.last_seen_epoch(id) == 0xDEADBEEFCAFEBABEull, "AC3: full 64-bit epoch");

        flat.mark_occurrence_stale(id);
        CHECK(flat.is_occurrence_stale(id) == 1, "AC3: occ stale");
        CHECK(flat.occurrence_stale_count() >= 1, "AC3: stale count");
        flat.clear_occurrence_stale(id);
        CHECK(flat.is_occurrence_stale(id) == 0, "AC3: occ cleared");

        flat.clear_verification_dirty(id);
        CHECK(flat.verification_dirty(id) == 0, "AC3: verification cleared");
    }

    // ── AC1: concurrent reader + writer (epoch + occ) ──────────────
    {
        std::println("\n--- #2440 AC1: concurrent stamp_epoch / mark_stale + readers ---");
        FlatAST flat;
        constexpr int kN = 16;
        std::vector<NodeId> ids;
        ids.reserve(kN);
        for (int i = 0; i < kN; ++i)
            ids.push_back(flat.add_node(NodeTag::LiteralInt));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> writes{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> epoch_hi_seen{0};

        std::vector<std::thread> threads;
        // 2 writers: stamp high-bit epoch + mark stale
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto id = ids[static_cast<std::size_t>(i % kN)];
                        // High-bit patterns: torn 32-bit half would corrupt.
                        const std::uint64_t ep =
                            (0xA5A5A5A500000000ull) | static_cast<std::uint64_t>(i & 0xFFFFFFFFu);
                        flat.stamp_last_seen_epoch(id, ep);
                        flat.mark_occurrence_stale(id);
                        if ((i & 3) == 0)
                            flat.clear_occurrence_stale(id);
                        writes.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 4 readers
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto id = ids[static_cast<std::size_t>(i % kN)];
                        const auto ep = flat.last_seen_epoch(id);
                        (void)flat.is_occurrence_stale(id);
                        // If non-zero, high half should be 0 or 0xA5A5A5A5 (no tear).
                        if (ep != 0) {
                            const auto hi = ep >> 32;
                            if (hi != 0 && hi != 0xA5A5A5A5ull)
                                err.fetch_add(1, std::memory_order_relaxed);
                            else if (hi == 0xA5A5A5A5ull)
                                epoch_hi_seen.fetch_add(1, std::memory_order_relaxed);
                        }
                        reads.fetch_add(1, std::memory_order_relaxed);
                        i += 4;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  writes={} reads={} epoch_hi_seen={} err={}", writes.load(), reads.load(),
                     epoch_hi_seen.load(), err.load());
        CHECK(writes.load() > 0, "AC1: writers progressed");
        CHECK(reads.load() > 0, "AC1: readers progressed");
        CHECK(epoch_hi_seen.load() > 0, "AC1: high-bit epochs observed intact");
        CHECK(err.load() == 0, "AC1: no tear / exceptions");
    }

    // ── AC2: 4 threads concurrent write+read on all 4 columns ──────
    {
        std::println("\n--- #2440 AC2: concurrent 4-column write+read ---");
        FlatAST flat;
        constexpr int kN = 8;
        std::vector<NodeId> ids;
        for (int i = 0; i < kN; ++i)
            ids.push_back(flat.add_node(NodeTag::LiteralInt));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ops{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto id = ids[static_cast<std::size_t>(i % kN)];
                        flat.apply_verify_dirty_bits(
                            id, static_cast<std::uint8_t>(FlatAST::kAssertionDirty |
                                                          FlatAST::kSvaDirty));
                        flat.apply_verification_dirty_bits(
                            id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty |
                                                          FlatAST::kAssertFailureDirty));
                        flat.stamp_last_seen_epoch(id, static_cast<std::uint64_t>(i) + 1);
                        flat.mark_occurrence_stale(id);
                        (void)flat.verify_dirty(id);
                        (void)flat.verification_dirty(id);
                        (void)flat.last_seen_epoch(id);
                        (void)flat.is_occurrence_stale(id);
                        (void)flat.is_verification_dirty(id);
                        if ((i & 7) == 0)
                            flat.clear_occurrence_stale(id);
                        ops.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  ops={} err={}", ops.load(), err.load());
        CHECK(ops.load() > 0, "AC2: concurrent ops progressed");
        CHECK(err.load() == 0, "AC2: no exceptions under concurrency");

        // Bits/epochs still coherent after storm
        for (auto id : ids) {
            const auto ep = flat.last_seen_epoch(id);
            CHECK(ep > 0 || flat.verify_dirty(id) != 0 || flat.verification_dirty(id) != 0 ||
                      flat.is_occurrence_stale(id) != 0,
                  "AC2: node has some concurrent state");
            (void)ep;
        }
        // Metric: first-set counts not wildly over-counted (≤ kN * reasons)
        CHECK(flat.verify_assertion_dirty_total() <= static_cast<std::uint64_t>(kN),
              "AC2: assertion metric ≤ node count");
        CHECK(flat.verification_coverage_feedback_total() <= static_cast<std::uint64_t>(kN),
              "AC2: feedback metric ≤ node count");
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2440") != std::string::npos, "source-cite #2440");
        CHECK(ast.find("std::atomic_ref<std::uint64_t>") != std::string::npos,
              "atomic_ref uint64 on epoch");
        CHECK(ast.find("std::atomic_ref<std::uint8_t>") != std::string::npos,
              "atomic_ref uint8 on dirty/stale");
        CHECK(ast.find("is_always_lock_free") != std::string::npos &&
                  ast.find("uint64") != std::string::npos,
              "is_always_lock_free assert present");
        CHECK(ast.find("fetch_or(reasons") != std::string::npos, "fetch_or verification path");
    }

    std::println("\n=== #2440 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_soa_column_atomic_2440();
}
#endif

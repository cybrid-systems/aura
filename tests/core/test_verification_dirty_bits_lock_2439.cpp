// @category: unit
// @reason: Issue #2439 — apply_verification_dirty_bits / apply_verify_dirty_bits
//          newly_set metric must not double-count under concurrent same-id apply.
//
//   AC1: concurrent apply_verification_dirty_bits(same_id, same_reasons) → +1 metric
//   AC2: concurrent apply_verify_dirty_bits + apply_verification (TSan-friendly)
//   AC3: bits correctly set after concurrent apply
//   AC4: single-thread / mark_dirty_verification still works

#include "test_harness.hpp"

#include <atomic>
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

int main() {
    std::println("=== Issue #2439: verification dirty bits lock (no metric double-count) ===");

    // ── AC4: single-thread baseline ────────────────────────────────
    {
        std::println("\n--- #2439 AC4: single-thread apply + mark_dirty_verification ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        CHECK(flat.verification_dirty(id) == 0, "AC4: clean verification_dirty");
        CHECK(flat.verification_coverage_feedback_total() == 0, "AC4: feedback 0");
        CHECK(flat.verification_assert_failure_total() == 0, "AC4: assert 0");

        flat.apply_verification_dirty_bits(
            id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty));
        CHECK((flat.verification_dirty(id) & FlatAST::kCoverageFeedbackDirty) != 0,
              "AC4: coverage bit set");
        CHECK(flat.verification_coverage_feedback_total() == 1, "AC4: feedback +1 once");
        // Re-apply same bit → no metric bump
        flat.apply_verification_dirty_bits(
            id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty));
        CHECK(flat.verification_coverage_feedback_total() == 1, "AC4: re-apply no double-count");

        flat.mark_dirty_verification(id);
        // kAssertFailure may newly set (+1); coverage already set
        CHECK(flat.verification_assert_failure_total() >= 1,
              "AC4: assert set via mark_dirty_verification");
        CHECK((flat.verification_dirty(id) & FlatAST::kAssertFailureDirty) != 0,
              "AC4: assert bit set");
        CHECK(flat.is_dirty(id) || flat.dirty(id) != 0, "AC4: general dirty mirrored");
    }

    // ── AC1: concurrent same-id same-reasons → metric +1 ───────────
    {
        std::println("\n--- #2439 AC1: concurrent apply_verification same id/reasons ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        const auto fb0 = flat.verification_coverage_feedback_total();
        const auto as0 = flat.verification_assert_failure_total();

        constexpr int kThreads = 4;
        constexpr int kIters = 500;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        const auto reasons = static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty |
                                                       FlatAST::kAssertFailureDirty);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kIters; ++i)
                    flat.apply_verification_dirty_bits(id, reasons);
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        const auto fb1 = flat.verification_coverage_feedback_total();
        const auto as1 = flat.verification_assert_failure_total();
        // Exactly one first-set per reason (not kThreads * kIters).
        CHECK(fb1 == fb0 + 1, "AC1: coverage metric +1 (no double-count)");
        CHECK(as1 == as0 + 1, "AC1: assert metric +1 (no double-count)");
        CHECK((flat.verification_dirty(id) & reasons) == reasons, "AC1: both bits set");
    }

    // ── AC2: concurrent verify_dirty + verification_dirty stress ───
    {
        std::println("\n--- #2439 AC2: concurrent apply_verify + apply_verification ---");
        FlatAST flat;
        constexpr int kN = 32;
        for (int i = 0; i < kN; ++i)
            (void)flat.add_node(NodeTag::LiteralInt);

        const auto ass0 = flat.verify_assertion_dirty_total();
        const auto cov0 = flat.verify_coverage_dirty_total();
        const auto fb0 = flat.verification_coverage_feedback_total();

        std::atomic<bool> start{false};
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) {
                }
                try {
                    for (int i = 0; i < 1000; ++i) {
                        const auto id = static_cast<NodeId>((t + i) % kN);
                        if ((i & 1) == 0)
                            flat.apply_verify_dirty_bits(
                                id, static_cast<std::uint8_t>(FlatAST::kAssertionDirty |
                                                              FlatAST::kCoverageDirty));
                        else
                            flat.apply_verification_dirty_bits(
                                id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty));
                        (void)flat.verify_dirty(id);
                        (void)flat.verification_dirty(id);
                    }
                } catch (...) {
                    errors.fetch_add(1);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        CHECK(errors.load() == 0, "AC2: no exceptions");
        // Metrics: at most one first-set per node per reason (≤ kN each).
        CHECK(flat.verify_assertion_dirty_total() - ass0 <= static_cast<std::uint64_t>(kN),
              "AC2: assertion total ≤ node count");
        CHECK(flat.verify_coverage_dirty_total() - cov0 <= static_cast<std::uint64_t>(kN),
              "AC2: coverage total ≤ node count");
        CHECK(flat.verification_coverage_feedback_total() - fb0 <= static_cast<std::uint64_t>(kN),
              "AC2: feedback total ≤ node count");
        // At least some bits set
        CHECK(flat.verify_assertion_dirty_total() > ass0, "AC2: assertion advanced");
        CHECK(flat.verification_coverage_feedback_total() > fb0, "AC2: feedback advanced");
    }

    // ── AC3: bits set after concurrent apply (same as AC1 bits check) ─
    {
        std::println("\n--- #2439 AC3: bit semantics after concurrent apply ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 200; ++i) {
                    flat.apply_verification_dirty_bits(
                        id, static_cast<std::uint8_t>(FlatAST::kCoverageFeedbackDirty));
                    flat.apply_verify_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kSvaDirty));
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        CHECK((flat.verification_dirty(id) & FlatAST::kCoverageFeedbackDirty) != 0,
              "AC3: verification coverage bit held");
        CHECK((flat.verify_dirty(id) & FlatAST::kSvaDirty) != 0, "AC3: verify SVA bit held");
        CHECK(flat.verification_coverage_feedback_total() == 1, "AC3: feedback once");
        CHECK(flat.verify_sva_dirty_total() == 1, "AC3: sva once");
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2439") != std::string::npos, "source-cite #2439");
        CHECK(ast.find("dirty_column_mtx_") != std::string::npos &&
                  ast.find("apply_verification_dirty_bits") != std::string::npos,
              "lock used in apply_verification");
        // newly_set computed under lock (unique_lock before newly_set)
        CHECK(ast.find("newly_set = static_cast<std::uint8_t>(reasons & ~verification_dirty_") !=
                  std::string::npos,
              "newly_set under exclusive path");
    }

    std::println("\n=== #2439 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

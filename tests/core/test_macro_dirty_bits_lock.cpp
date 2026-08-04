// @category: unit
// @reason: Issue #2441 — apply_macro_dirty_bits newly_set metric must not
//          double-count under concurrent same-id apply (atomic fetch_or).
//
//   AC1: concurrent apply_macro_dirty_bits(same_id, same_reasons) → +1 metric
//   AC2: 4 threads concurrent write+read (TSan-friendly)
//   AC3: bits correctly set after concurrent apply
//   AC4: single-thread / clear / count / clone-style paths still work

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

int run_test_macro_dirty_bits_lock() {
    std::println("=== Issue #2441: macro_dirty bits lock (no metric double-count) ===");

    // ── AC4: single-thread baseline ────────────────────────────────
    {
        std::println("\n--- #2441 AC4: single-thread apply + clear + count ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        CHECK(flat.macro_dirty(id) == 0, "AC4: clean macro_dirty");
        CHECK(flat.macro_expansion_dirty_total() == 0, "AC4: expansion 0");
        CHECK(flat.macro_self_modify_dirty_total() == 0, "AC4: self-modify 0");
        CHECK(flat.macro_dirty_count() == 0, "AC4: count 0");

        flat.apply_macro_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
        CHECK((flat.macro_dirty(id) & FlatAST::kMacroExpansion) != 0, "AC4: expansion bit set");
        CHECK(flat.macro_expansion_dirty_total() == 1, "AC4: expansion +1 once");
        // Re-apply same bit → no metric bump
        flat.apply_macro_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
        CHECK(flat.macro_expansion_dirty_total() == 1, "AC4: re-apply no double-count");

        flat.apply_macro_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kMacroSelfModify));
        CHECK((flat.macro_dirty(id) & FlatAST::kMacroSelfModify) != 0, "AC4: self-modify bit");
        CHECK(flat.macro_self_modify_dirty_total() == 1, "AC4: self-modify +1");
        CHECK(flat.macro_dirty_count() >= 1, "AC4: count ≥ 1");
        CHECK(flat.is_dirty(id) || flat.dirty(id) != 0, "AC4: general dirty mirrored");

        flat.clear_macro_dirty_all();
        CHECK(flat.macro_dirty(id) == 0, "AC4: clear_all zeros");
        CHECK(flat.macro_dirty_count() == 0, "AC4: count 0 after clear");
    }

    // ── AC1: concurrent same-id same-reasons → metric +1 ───────────
    {
        std::println("\n--- #2441 AC1: concurrent apply_macro same id/reasons ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        const auto e0 = flat.macro_expansion_dirty_total();
        const auto s0 = flat.macro_self_modify_dirty_total();

        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 500; ++i) {
                    flat.apply_macro_dirty_bits(
                        id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion |
                                                      FlatAST::kMacroSelfModify));
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        CHECK(flat.macro_expansion_dirty_total() - e0 == 1,
              "AC1: expansion metric +1 (no double-count)");
        CHECK(flat.macro_self_modify_dirty_total() - s0 == 1,
              "AC1: self-modify metric +1 (no double-count)");
        CHECK((flat.macro_dirty(id) & FlatAST::kMacroExpansion) != 0, "AC1: expansion bit held");
        CHECK((flat.macro_dirty(id) & FlatAST::kMacroSelfModify) != 0, "AC1: self-modify bit held");
    }

    // ── AC2: concurrent multi-id write + read ──────────────────────
    {
        std::println("\n--- #2441 AC2: concurrent multi-id write+read ---");
        FlatAST flat;
        constexpr int kN = 12;
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
                        flat.apply_macro_dirty_bits(
                            id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
                        flat.apply_macro_dirty_bits(
                            id, static_cast<std::uint8_t>(FlatAST::kMacroSelfModify));
                        (void)flat.macro_dirty(id);
                        (void)flat.macro_dirty_count();
                        ops.fetch_add(1, std::memory_order_relaxed);
                        ++i;
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

        std::println("  ops={} err={}", ops.load(), err.load());
        CHECK(ops.load() > 0, "AC2: concurrent ops progressed");
        CHECK(err.load() == 0, "AC2: no exceptions");
        // Metrics: at most one first-set per node per reason
        CHECK(flat.macro_expansion_dirty_total() <= static_cast<std::uint64_t>(kN),
              "AC2: expansion total ≤ node count");
        CHECK(flat.macro_self_modify_dirty_total() <= static_cast<std::uint64_t>(kN),
              "AC2: self-modify total ≤ node count");
        CHECK(flat.macro_expansion_dirty_total() > 0, "AC2: expansion advanced");
    }

    // ── AC3: bit semantics after concurrent apply ──────────────────
    {
        std::println("\n--- #2441 AC3: bit semantics after concurrent apply ---");
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 200; ++i) {
                    flat.apply_macro_dirty_bits(
                        id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        CHECK((flat.macro_dirty(id) & FlatAST::kMacroExpansion) != 0, "AC3: expansion bit held");
        CHECK(flat.macro_expansion_dirty_total() == 1, "AC3: expansion once");
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2441") != std::string::npos, "source-cite #2441");
        CHECK(ast.find("apply_macro_dirty_bits") != std::string::npos &&
                  ast.find("dirty_column_mtx_") != std::string::npos,
              "lock used in apply_macro");
        CHECK(ast.find("fetch_or(reasons") != std::string::npos &&
                  ast.find("newly_set = static_cast<std::uint8_t>(reasons & ~prev)") !=
                      std::string::npos,
              "newly_set via fetch_or prev");
    }

    std::println("\n=== #2441 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_macro_dirty_bits_lock();
}
#endif

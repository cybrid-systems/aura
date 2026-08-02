// @category: unit
// @reason: Issue #2442 — clear_macro_dirty_all concurrent with macro_dirty(id)
//          readers must not tear (exclusive clear + shared atomic load).
//
//   AC1: concurrent clear_macro_dirty_all + macro_dirty(id) no torn reads
//   AC2: 4 threads clear + read (TSan-friendly)
//   AC3: after clear completes, all bits are 0

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

int run_test_clear_macro_dirty_concurrent_2442() {
    std::println("=== Issue #2442: clear_macro_dirty_all concurrent with readers ===");

    // ── AC3: single-thread clear semantics ─────────────────────────
    {
        std::println("\n--- #2442 AC3: all bits cleared after clear_macro_dirty_all ---");
        FlatAST flat;
        constexpr int kN = 8;
        std::vector<NodeId> ids;
        for (int i = 0; i < kN; ++i) {
            const auto id = flat.add_node(NodeTag::LiteralInt);
            ids.push_back(id);
            flat.apply_macro_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion |
                                                                      FlatAST::kMacroSelfModify));
        }
        for (auto id : ids) {
            CHECK(flat.macro_dirty(id) != 0, "AC3: dirty before clear");
        }
        CHECK(flat.macro_dirty_count() == static_cast<std::size_t>(kN), "AC3: count = N");

        flat.clear_macro_dirty_all();

        for (auto id : ids) {
            CHECK(flat.macro_dirty(id) == 0, "AC3: clean after clear");
        }
        CHECK(flat.macro_dirty_count() == 0, "AC3: count 0 after clear");
    }

    // ── AC1: concurrent clear + macro_dirty(id) ────────────────────
    {
        std::println("\n--- #2442 AC1: concurrent clear + macro_dirty readers ---");
        FlatAST flat;
        constexpr int kN = 16;
        std::vector<NodeId> ids;
        for (int i = 0; i < kN; ++i) {
            const auto id = flat.add_node(NodeTag::LiteralInt);
            ids.push_back(id);
            flat.apply_macro_dirty_bits(id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> clears{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> reapplies{0};
        std::atomic<std::uint64_t> err{0};
        // Readers only observe 0 or valid reason bits (no garbage).
        constexpr std::uint8_t kValidMask =
            static_cast<std::uint8_t>(FlatAST::kMacroExpansion | FlatAST::kMacroSelfModify);

        std::vector<std::thread> threads;
        // 1 clearer
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    flat.clear_macro_dirty_all();
                    clears.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // 1 re-applier (keep column dirty so clear has work)
        threads.emplace_back([&] {
            int i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    const auto id = ids[static_cast<std::size_t>(i % kN)];
                    flat.apply_macro_dirty_bits(
                        id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion |
                                                      FlatAST::kMacroSelfModify));
                    reapplies.fetch_add(1, std::memory_order_relaxed);
                    ++i;
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // 4 readers
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto id = ids[static_cast<std::size_t>(i % kN)];
                        const auto v = flat.macro_dirty(id);
                        // Only 0 or subset of known bits — never high garbage bits.
                        if ((v & static_cast<std::uint8_t>(~kValidMask)) != 0)
                            err.fetch_add(1, std::memory_order_relaxed);
                        (void)flat.macro_dirty_count();
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

        std::println("  clears={} reapplies={} reads={} err={}", clears.load(), reapplies.load(),
                     reads.load(), err.load());
        CHECK(clears.load() > 0, "AC1: clear progressed");
        CHECK(reads.load() > 0, "AC1: readers progressed");
        CHECK(reapplies.load() > 0, "AC1: re-applies progressed");
        CHECK(err.load() == 0, "AC1: no tear / invalid bits / exceptions");
    }

    // ── AC2: 4 threads mixed clear + read ──────────────────────────
    {
        std::println("\n--- #2442 AC2: 4 threads clear+read mix ---");
        FlatAST flat;
        constexpr int kN = 8;
        std::vector<NodeId> ids;
        for (int i = 0; i < kN; ++i) {
            ids.push_back(flat.add_node(NodeTag::LiteralInt));
            flat.apply_macro_dirty_bits(ids.back(),
                                        static_cast<std::uint8_t>(FlatAST::kMacroSelfModify));
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ops{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        if ((i & 3) == 0)
                            flat.clear_macro_dirty_all();
                        else {
                            const auto id = ids[static_cast<std::size_t>(i % kN)];
                            flat.apply_macro_dirty_bits(
                                id, static_cast<std::uint8_t>(FlatAST::kMacroExpansion));
                            (void)flat.macro_dirty(id);
                        }
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
        CHECK(err.load() == 0, "AC2: no exceptions under concurrency");

        // Final clear → all clean
        flat.clear_macro_dirty_all();
        for (auto id : ids)
            CHECK(flat.macro_dirty(id) == 0, "AC2: final clear zeros node");
        CHECK(flat.macro_dirty_count() == 0, "AC2: final count 0");
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2442") != std::string::npos, "source-cite #2442");
        CHECK(ast.find("clear_macro_dirty_all") != std::string::npos &&
                  ast.find("atomic_ref<std::uint8_t>") != std::string::npos,
              "clear uses atomic_ref");
        CHECK(ast.find("unique_lock") != std::string::npos, "exclusive lock on clear path");
    }

    std::println("\n=== #2442 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_clear_macro_dirty_concurrent_2442();
}
#endif

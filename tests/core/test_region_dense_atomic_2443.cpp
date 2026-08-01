// @category: unit
// @reason: Issue #2443 — region_by_sym_dense_ / region_by_lambda_dense_
//          concurrent-safe via region_table_mtx_ + atomic_ref.
//
//   AC1: concurrent writer + reader does not tear dense uint8 cells
//   AC2: 4 threads concurrent set + get (TSan-friendly)
//   AC3: region classification semantics preserved (encoding region+1)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
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
using aura::ast::SymId;
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
    std::println("=== Issue #2443: region dense SoA atomic_ref ===");

    // ── AC3: single-thread semantics (encoding region+1) ───────────
    {
        std::println("\n--- #2443 AC3: set/get region classification ---");
        FlatAST flat;
        const SymId sym = 42;
        const auto lam = flat.add_node(NodeTag::Lambda);

        CHECK(!flat.get_function_region_for_sym(sym).has_value(), "AC3: sym unset");
        CHECK(!flat.get_function_region_for_lambda(lam).has_value(), "AC3: lambda unset");

        flat.set_function_region(sym, 0); // region 0 → dense stores 1
        auto r0 = flat.get_function_region_for_sym(sym);
        CHECK(r0.has_value() && *r0 == 0, "AC3: region 0 round-trip");

        flat.set_function_region(sym, 7);
        auto r7 = flat.get_function_region_for_sym(sym);
        CHECK(r7.has_value() && *r7 == 7, "AC3: region 7 round-trip");

        flat.set_function_region_lambda(lam, 3);
        auto rl = flat.get_function_region_for_lambda(lam);
        CHECK(rl.has_value() && *rl == 3, "AC3: lambda region 3");

        // Overwrite preserves latest
        flat.set_function_region_lambda(lam, 9);
        CHECK(flat.get_function_region_for_lambda(lam).value_or(0) == 9, "AC3: lambda overwrite");
    }

    // ── AC1: concurrent writer + reader (no invalid region bytes) ──
    {
        std::println("\n--- #2443 AC1: concurrent set + get (dense path) ---");
        FlatAST flat;
        constexpr int kN = 32;
        // Pre-create lambda nodes so NodeIds are stable for dense index.
        std::vector<NodeId> lams;
        lams.reserve(kN);
        for (int i = 0; i < kN; ++i)
            lams.push_back(flat.add_node(NodeTag::Lambda));

        // Valid region values we publish (0..15) — reader rejects others.
        constexpr std::uint8_t kMaxRegion = 15;

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> writes{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> hits{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        // 2 writers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto idx = static_cast<std::size_t>(i % kN);
                        const SymId sym = static_cast<SymId>(1000 + idx);
                        const std::uint8_t reg = static_cast<std::uint8_t>((i + t) & kMaxRegion);
                        flat.set_function_region(sym, reg);
                        flat.set_function_region_lambda(lams[idx], reg);
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
                        const auto idx = static_cast<std::size_t>(i % kN);
                        const SymId sym = static_cast<SymId>(1000 + idx);
                        auto rs = flat.get_function_region_for_sym(sym);
                        auto rl = flat.get_function_region_for_lambda(lams[idx]);
                        if (rs.has_value()) {
                            if (*rs > kMaxRegion)
                                err.fetch_add(1, std::memory_order_relaxed);
                            else
                                hits.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (rl.has_value()) {
                            if (*rl > kMaxRegion)
                                err.fetch_add(1, std::memory_order_relaxed);
                            else
                                hits.fetch_add(1, std::memory_order_relaxed);
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

        std::println("  writes={} reads={} hits={} err={}", writes.load(), reads.load(),
                     hits.load(), err.load());
        CHECK(writes.load() > 0, "AC1: writers progressed");
        CHECK(reads.load() > 0, "AC1: readers progressed");
        CHECK(hits.load() > 0, "AC1: dense hits observed");
        CHECK(err.load() == 0, "AC1: no tear / invalid region / exceptions");
    }

    // ── AC2: 4 threads mixed set/get ───────────────────────────────
    {
        std::println("\n--- #2443 AC2: 4 threads concurrent write+read ---");
        FlatAST flat;
        constexpr int kN = 12;
        std::vector<NodeId> lams;
        for (int i = 0; i < kN; ++i)
            lams.push_back(flat.add_node(NodeTag::Lambda));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ops{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto idx = static_cast<std::size_t>(i % kN);
                        const SymId sym = static_cast<SymId>(2000 + idx);
                        flat.set_function_region(sym, static_cast<std::uint8_t>(i & 7));
                        flat.set_function_region_lambda(lams[idx],
                                                        static_cast<std::uint8_t>((i + 1) & 7));
                        (void)flat.get_function_region_for_sym(sym);
                        (void)flat.get_function_region_for_lambda(lams[idx]);
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

        // Final values coherent for each key
        for (int i = 0; i < kN; ++i) {
            const SymId sym = static_cast<SymId>(2000 + i);
            auto rs = flat.get_function_region_for_sym(sym);
            auto rl = flat.get_function_region_for_lambda(lams[static_cast<std::size_t>(i)]);
            CHECK(rs.has_value() && *rs <= 7, "AC2: sym region in range");
            CHECK(rl.has_value() && *rl <= 7, "AC2: lambda region in range");
        }
    }

    // Source-cite
    {
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2443") != std::string::npos, "source-cite #2443");
        CHECK(ast.find("region_table_mtx_") != std::string::npos, "region_table_mtx_ present");
        CHECK(ast.find("region_by_sym_dense_") != std::string::npos &&
                  ast.find("atomic_ref<std::uint8_t>") != std::string::npos,
              "dense atomic_ref");
        CHECK(ast.find("get_function_region_for_sym") != std::string::npos &&
                  ast.find("shared_lock") != std::string::npos,
              "shared lock on get");
    }

    std::println("\n=== #2443 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

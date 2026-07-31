// @category: unit
// @reason: Issue #2421 — restamp_lazy_align_enabled_ is std::atomic<bool>.
//
//   AC1: flag is atomic (store/load with acquire/release)
//   AC2: concurrent is_valid + enabled flag safe
//   AC3: explicit memory orders (source-cite via linter)
//   AC4: wrap-restamp semantics preserved (#2122 incremental enables flag)

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
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;
using aura::test::g_failed;
using aura::test::g_passed;

// Same helper as test_incremental_restamp_2061 (#2061/#2122).
void force_one_wrap(FlatAST& ast) {
    constexpr std::uint64_t kBumpsPerWrap = 65536;
    for (std::uint64_t i = 0; i < kBumpsPerWrap; ++i)
        ast.bump_generation();
}

} // namespace

int main() {
    std::println("=== Issue #2421: restamp_lazy_align_enabled_ atomic ===");

    // ── AC1 default off ────────────────────────────────────────────
    {
        std::println("\n--- #2421 AC1: default false ---");
        FlatAST flat;
        (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        CHECK(!flat.restamp_lazy_align_enabled(), "AC1: default off");
    }

    // ── AC4 incremental wrap enables flag ──────────────────────────
    {
        std::println("\n--- #2421 AC4: incremental wrap enables lazy align ---");
        FlatAST cone;
        constexpr int kN = 4000;
        for (int i = 0; i < kN; ++i)
            cone.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        for (int i = 0; i < 4; ++i)
            cone.mark_dirty(static_cast<aura::ast::NodeId>(i));
        force_one_wrap(cone);
        cone.restamp_all_node_generations();
        CHECK(cone.restamp_lazy_align_enabled(), "AC4: lazy align on after incremental wrap");
        CHECK(cone.is_valid(static_cast<aura::ast::NodeId>(100)), "AC4: is_valid lazy-align path");
        const auto align0 = cone.restamp_lazy_align_total();
        (void)cone.make_ref(static_cast<aura::ast::NodeId>(200));
        CHECK(cone.restamp_lazy_align_total() >= align0, "AC4: make_ref may lazy-align");
    }

    // ── AC2 concurrent is_valid while enabled ──────────────────────
    {
        std::println("\n--- #2421 AC2: concurrent is_valid while lazy-align on ---");
        FlatAST cone;
        for (int i = 0; i < 2000; ++i)
            cone.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        cone.mark_dirty(0);
        force_one_wrap(cone);
        cone.restamp_all_node_generations();
        CHECK(cone.restamp_lazy_align_enabled(), "AC2: flag enabled");

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> ok{0};
        std::atomic<std::uint64_t> err{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        for (int i = 0; i < 50; ++i) {
                            if (cone.is_valid(static_cast<aura::ast::NodeId>(i % 1000)))
                                ok.fetch_add(1, std::memory_order_relaxed);
                        }
                        (void)cone.make_ref(static_cast<aura::ast::NodeId>(10));
                        (void)cone.restamp_lazy_align_enabled();
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        std::println("  ok={} err={}", ok.load(), err.load());
        CHECK(ok.load() > 0, "AC2: concurrent is_valid progressed");
        CHECK(err.load() == 0, "AC2: no exceptions");
    }

    // ── AC3 full restamp clears flag ───────────────────────────────
    {
        std::println("\n--- #2421 AC3: full restamp clears flag ---");
        FlatAST dense;
        for (int i = 0; i < 100; ++i)
            dense.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        for (int i = 0; i < 50; ++i)
            dense.mark_dirty(static_cast<aura::ast::NodeId>(i));
        force_one_wrap(dense);
        dense.restamp_all_node_generations();
        CHECK(!dense.restamp_lazy_align_enabled(), "AC3: full restamp disables lazy align");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

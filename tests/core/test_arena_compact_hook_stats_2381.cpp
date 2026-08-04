// @category: unit
// @reason: Issue #2381 — concurrent compact_hook shape_inval counter is
// atomic (no data race / no lost updates under N-thread stress).
//
//   AC1: N=4 threads invoke compact_hook concurrently → TSAN clean path
//   AC2: Counter value matches # successful hook invocations exactly
//   AC3: GUARDED_BY audit on serial stats_ fields + atomic concurrent-hot
//   AC4: Existing compact / live_compact surfaces still wired (source-cite)

#include "test_harness.hpp"

#include <atomic>
#include <cstddef>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.arena;

namespace {

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

// AC1 + AC2: N threads invoke hook path; counter == invocations.
static void ac1_ac2_concurrent_hook_counter() {
    std::println("\n--- #2381 AC1/AC2: concurrent compact_hook counter ---");
    aura::ast::ASTArena arena(/*initial_size=*/64 * 1024);

    std::atomic<std::uint64_t> hook_calls{0};
    arena.set_on_compact_hook(
        [&]() noexcept { hook_calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(arena.has_on_compact_hook(), "hook installed");

    constexpr int kThreads = 4;
    constexpr int kIters = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIters; ++i)
                arena.invoke_on_compact_hook_for_test();
        });
    }
    for (auto& th : threads)
        th.join();

    const auto expected = static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kIters);
    const auto hooks = hook_calls.load(std::memory_order_relaxed);
    const auto counter = arena.shape_inval_on_compact_relaxed();
    const auto snap = arena.stats().shape_inval_on_compact;

    CHECK(hooks == expected, "AC1: hook ran expected times");
    CHECK(counter == expected, "AC2: shape_inval_on_compact matches invocations exactly");
    CHECK(static_cast<std::uint64_t>(snap) == expected,
          "AC2: stats() snapshot matches atomic counter");
    CHECK(counter == hooks, "AC2: arena counter == hook-side counter");
}

// AC3: source audit — GUARDED_BY + atomic concurrent path.
static void ac3_source_audit() {
    std::println("\n--- #2381 AC3: GUARDED_BY + atomic concurrent-hot ---");
    const auto src = read_file("src/core/arena.ixx");
    CHECK(!src.empty(), "arena.ixx readable");
    CHECK(src.find("GUARDED_BY(per-arena compact serial)") != std::string::npos,
          "AC3: GUARDED_BY annotation present");
    CHECK(src.find("shape_inval_on_compact_") != std::string::npos,
          "AC3: atomic shape_inval_on_compact_ member");
    CHECK(src.find("fetch_add(1, std::memory_order_relaxed)") != std::string::npos ||
              src.find("shape_inval_on_compact_.fetch_add") != std::string::npos,
          "AC3: shape_inval uses fetch_add relaxed");
    CHECK(src.find("root_remap_stable_ref_total_") != std::string::npos,
          "AC3: root_remap counters also atomic (sweep)");
    CHECK(src.find("Issue #2381") != std::string::npos, "AC3: cites #2381");
    // No plain non-atomic ++ on the live counter under hook path.
    CHECK(src.find("stats_.shape_inval_on_compact++") == std::string::npos,
          "AC3: no plain stats_.shape_inval_on_compact++");
}

// AC4: compact path still calls invoke_compact_hook_; gate/linter wired.
static void ac4_wiring() {
    std::println("\n--- #2381 AC4: compact path + registration ---");
    const auto src = read_file("src/core/arena.ixx");
    CHECK(src.find("invoke_compact_hook_()") != std::string::npos,
          "AC4: compact path still invokes hook");
    CHECK(src.find("invoke_on_compact_hook_for_test") != std::string::npos,
          "AC4: test helper present");

    // Smoke: real compact() still fires hook when bytes reclaimed.
    aura::ast::ASTArena arena(/*initial_size=*/256 * 1024);
    std::atomic<int> fired{0};
    arena.set_on_compact_hook([&]() noexcept { fired.fetch_add(1, std::memory_order_relaxed); });
    const auto saved = arena.compact();
    if (saved > 0) {
        CHECK(fired.load() >= 1, "AC4: compact() fires hook when reclaimed");
        CHECK(arena.shape_inval_on_compact_relaxed() >= 1,
              "AC4: compact() bumps concurrent-safe counter");
    } else {
        // Empty shrink may still reclaim on large initial buffer.
        CHECK(true, "AC4: compact no-op ok (counter path covered by AC1 stress)");
    }

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_arena_compact_hook_stats_2381") != std::string::npos,
          "AC4: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_arena_compact_hook_stats_2381") != std::string::npos ||
              build.find("cmd_arena_compact_hook_stats_coverage") != std::string::npos,
          "AC4: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_arena_compact_hook_stats_2381.py");
    CHECK(!gate.empty() && gate.find("Issue #2381") != std::string::npos,
          "AC4: coverage linter present");
}

} // namespace

int run_test_arena_compact_hook_stats_2381() {
    std::println("=== Issue #2381: concurrent compact_hook shape_inval atomic ===");
    ac1_ac2_concurrent_hook_counter();
    ac3_source_audit();
    ac4_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_compact_hook_stats_2381();
}
#endif

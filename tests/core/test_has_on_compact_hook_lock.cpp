// @category: unit
// @reason: Issue #2383 — has_on_compact_hook locks hook_mtx_ (parity with
// has_on_layout_change / has_root_remap_callback). Prevents TSAN race vs set.
//
//   AC1: All three has_* methods take their respective mutexes (source)
//   AC2: Concurrent set + has under N threads completes without crash
//   AC3: Existing install/take/has semantics still correct

#include "arena_nonalloc_hooks.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
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

// Extract body of has_on_compact_hook for lock-pattern checks.
static std::string has_method_body(const std::string& src, const char* name) {
    const auto key = std::string("bool ") + name + "() const noexcept";
    auto i = src.find(key);
    if (i == std::string::npos)
        return {};
    auto brace = src.find('{', i);
    if (brace == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t j = brace; j < src.size(); ++j) {
        if (src[j] == '{')
            ++depth;
        else if (src[j] == '}') {
            --depth;
            if (depth == 0)
                return src.substr(brace, j - brace + 1);
        }
    }
    return {};
}

// AC1: all three has_* take their mutex.
static void ac1_source_lock_parity() {
    std::println("\n--- #2383 AC1: has_* lock pattern parity ---");
    const auto src = read_file("src/core/arena.ixx");
    CHECK(!src.empty(), "arena.ixx readable");
    CHECK(src.find("Issue #2383") != std::string::npos, "AC1: cites #2383");

    const auto compact = has_method_body(src, "has_on_compact_hook");
    const auto layout = has_method_body(src, "has_on_layout_change");
    const auto root = has_method_body(src, "has_root_remap_callback");
    CHECK(!compact.empty(), "AC1: has_on_compact_hook body found");
    CHECK(!layout.empty(), "AC1: has_on_layout_change body found");
    CHECK(!root.empty(), "AC1: has_root_remap_callback body found");

    CHECK(compact.find("hook_mtx_") != std::string::npos,
          "AC1: has_on_compact_hook takes hook_mtx_");
    CHECK(compact.find("lock_guard") != std::string::npos ||
              compact.find("std::lock_guard") != std::string::npos,
          "AC1: has_on_compact_hook uses lock_guard");
    CHECK(layout.find("on_layout_change_mtx_") != std::string::npos,
          "AC1: has_on_layout_change takes on_layout_change_mtx_");
    CHECK(root.find("root_remap_mtx_") != std::string::npos,
          "AC1: has_root_remap_callback takes root_remap_mtx_");
}

// AC2: concurrent set + has (TSAN-friendly path).
static void ac2_concurrent_set_has() {
    std::println("\n--- #2383 AC2: concurrent set + has ---");
    aura::ast::ASTArena arena(/*initial_size=*/8 * 1024);
    std::atomic<int> errors{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> thr;
    thr.reserve(6);
    // Setters for all three hook families.
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            try {
                arena.set_on_compact_hook(&arena_hook_compact_nop, nullptr);
                (void)arena.take_on_compact_hook();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            try {
                arena.set_on_layout_change(&arena_hook_layout_nop, nullptr);
                (void)arena.take_on_layout_change();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            try {
                arena.set_root_remap_callback(&arena_hook_root_remap_nop, nullptr);
                (void)arena.take_root_remap_callback();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    // has_* readers
    for (int i = 0; i < 3; ++i) {
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                try {
                    (void)arena.has_on_compact_hook();
                    (void)arena.has_on_layout_change();
                    (void)arena.has_root_remap_callback();
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();
    CHECK(errors.load() == 0, "AC2: no exceptions under concurrent set+has");
}

// AC3: install / take / has semantics.
static void ac3_semantics() {
    std::println("\n--- #2383 AC3: install/take/has semantics ---");
    aura::ast::ASTArena arena(/*initial_size=*/8 * 1024);
    CHECK(!arena.has_on_compact_hook(), "fresh: no compact hook");
    CHECK(!arena.has_on_layout_change(), "fresh: no layout hook");
    CHECK(!arena.has_root_remap_callback(), "fresh: no root_remap");

    arena.set_on_compact_hook(&arena_hook_compact_nop, nullptr);
    arena.set_on_layout_change(&arena_hook_layout_nop, nullptr);
    arena.set_root_remap_callback(&arena_hook_root_remap_nop, nullptr);
    CHECK(arena.has_on_compact_hook(), "after set: compact");
    CHECK(arena.has_on_layout_change(), "after set: layout");
    CHECK(arena.has_root_remap_callback(), "after set: root_remap");

    (void)arena.take_on_compact_hook();
    (void)arena.take_on_layout_change();
    (void)arena.take_root_remap_callback();
    CHECK(!arena.has_on_compact_hook(), "after take: compact empty");
    CHECK(!arena.has_on_layout_change(), "after take: layout empty");
    CHECK(!arena.has_root_remap_callback(), "after take: root_remap empty");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_has_on_compact_hook_lock") != std::string::npos,
          "AC3: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_has_on_compact_hook_lock_2383") != std::string::npos ||
              build.find("cmd_has_on_compact_hook_lock_coverage") != std::string::npos,
          "AC3: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_has_on_compact_hook_lock_2383.py");
    CHECK(!gate.empty() && gate.find("Issue #2383") != std::string::npos,
          "AC3: coverage linter present");
}

// ── Issue #3124: non-allocating {fn,ctx} slots ──
static void ac3124_1_no_std_function_api() {
    std::println("\n--- #3124 AC1: APIs are function-pointer + ctx ---");
    const auto src = read_file("src/core/arena.ixx");
    CHECK(src.find("CompactHookFn") != std::string::npos, "AC1: CompactHookFn");
    CHECK(src.find("LayoutChangeHookFn") != std::string::npos, "AC1: LayoutChangeHookFn");
    CHECK(src.find("RootRemapHookFn") != std::string::npos, "AC1: RootRemapHookFn");
    CHECK(src.find("kArenaCompactHookSlots = 4") != std::string::npos, "AC1: 4 compact slots");
    CHECK(src.find("kNonAllocatingArenaHooksIssue = 3124") != std::string::npos, "AC1: stamp");
    CHECK(src.find("set_on_compact_hook(std::function") == std::string::npos,
          "AC1: compact no std::function");
    CHECK(src.find("set_on_layout_change(LiveCompactLayoutChangeCallback") == std::string::npos ||
              src.find("LayoutChangeHookFn fn") != std::string::npos,
          "AC1: layout is fn+ctx");
}

static void ac3124_2_two_slots_fire() {
    std::println("\n--- #3124 AC2: two compact slots fire once each ---");
    aura::ast::ASTArena arena(/*initial_size=*/16 * 1024);
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    arena.set_on_compact_hook(&arena_hook_compact_bump_i32, &a);
    arena.set_on_compact_hook(&arena_hook_compact_bump_i32, &b);
    CHECK(arena.has_on_compact_hook(), "AC2: hooks installed");
    arena.invoke_on_compact_hook_for_test();
    CHECK(a.load() == 1, "AC2: first slot fired once");
    CHECK(b.load() == 1, "AC2: second slot fired once");
}

static void ac3124_5_source_and_linter() {
    std::println("\n--- #3124 AC5: source-cite + linter ---");
    const auto t = read_file("tests/core/test_has_on_compact_hook_lock.cpp");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3124_1_no_std_function_api") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac3124_2_two_slots_fire") != std::string::npos, "AC5: AC2 test");
    CHECK(build.find("check_nonalloc_arena_hooks_3124") != std::string::npos, "AC5: build.py");
    CHECK(read_file("tests/core/test_issue_3124.cpp").empty(), "AC5: no test_issue_3124.cpp");
    std::ifstream design("docs/design/3124-nonalloc-hooks.md");
    if (!design)
        design.open("../docs/design/3124-nonalloc-hooks.md");
    CHECK(!design.good(), "AC5: no docs/design/3124-* per #1655");
}

} // namespace

int run_test_has_on_compact_hook_lock() {
    std::println("=== Issue #2383: has_on_compact_hook lock parity ===");
    ac1_source_lock_parity();
    ac2_concurrent_set_has();
    ac3_semantics();
    ac3124_1_no_std_function_api();
    ac3124_2_two_slots_fire();
    ac3124_5_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_has_on_compact_hook_lock();
}
#endif

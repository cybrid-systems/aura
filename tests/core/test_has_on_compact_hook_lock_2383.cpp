// @category: unit
// @reason: Issue #2383 — has_on_compact_hook locks hook_mtx_ (parity with
// has_on_layout_change / has_root_remap_callback). Prevents TSAN race vs set.
//
//   AC1: All three has_* methods take their respective mutexes (source)
//   AC2: Concurrent set + has under N threads completes without crash
//   AC3: Existing install/take/has semantics still correct

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
                arena.set_on_compact_hook([]() noexcept {});
                (void)arena.take_on_compact_hook();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            try {
                arena.set_on_layout_change([](std::uint64_t, std::uint64_t) noexcept {});
                (void)arena.take_on_layout_change();
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            try {
                arena.set_root_remap_callback(
                    [](std::uint64_t, std::uint64_t, std::unordered_map<void*, void*> const&,
                       std::size_t&, std::size_t&, std::size_t&, std::size_t&) {});
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

    arena.set_on_compact_hook([]() noexcept {});
    arena.set_on_layout_change([](std::uint64_t, std::uint64_t) noexcept {});
    arena.set_root_remap_callback([](std::uint64_t, std::uint64_t,
                                     std::unordered_map<void*, void*> const&, std::size_t&,
                                     std::size_t&, std::size_t&, std::size_t&) {});
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
    CHECK(cmake.find("test_has_on_compact_hook_lock_2383") != std::string::npos,
          "AC3: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_has_on_compact_hook_lock_2383") != std::string::npos ||
              build.find("cmd_has_on_compact_hook_lock_coverage") != std::string::npos,
          "AC3: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_has_on_compact_hook_lock_2383.py");
    CHECK(!gate.empty() && gate.find("Issue #2383") != std::string::npos,
          "AC3: coverage linter present");
}

} // namespace

int run_test_has_on_compact_hook_lock_2383() {
    std::println("=== Issue #2383: has_on_compact_hook lock parity ===");
    ac1_source_lock_parity();
    ac2_concurrent_set_has();
    ac3_semantics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_has_on_compact_hook_lock_2383();
}
#endif

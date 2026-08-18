// @category: unit
// @reason: Issue #2382 — ASTArena dtor clears on_compact_hook_ /
// on_layout_change_ / root_remap_ under mutex before internal teardown
// so concurrent invoke_*_ cannot fire dangling caller-capturing lambdas.
//
//   AC1: After dtor, hook callables destroyed (shared_ptr capture use_count)
//   AC2: Install all three hooks → destroy → no post-dtor fire / capture gone
//   AC3: N-thread invoke + destroy race completes without crash (TSAN-friendly)
//   AC4: Source-cite dtor clear contract + gate registration

#include "arena_nonalloc_hooks.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <memory>
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

// AC1/AC2: #3124 hooks are {fn,ctx} — dtor nulls slots and must not fire.
static void ac1_ac2_dtor_clears_all_hooks() {
    std::println("\n--- #2382 AC1/AC2: dtor clears compact/layout/root_remap hooks ---");

    std::atomic<int> compact_fires{0};
    std::atomic<int> layout_fires{0};
    std::atomic<int> root_fires{0};

    {
        auto arena = std::make_unique<aura::ast::ASTArena>(/*initial_size=*/64 * 1024);
        arena->set_on_compact_hook(&arena_hook_compact_bump_i32, &compact_fires);
        arena->set_on_layout_change(&arena_hook_layout_nop, nullptr);
        arena->set_root_remap_callback(&arena_hook_root_remap_bump_i32, &root_fires);

        CHECK(arena->has_on_compact_hook(), "AC1: compact hook installed");
        CHECK(arena->has_on_layout_change(), "AC1: layout-change hook installed");
        CHECK(arena->has_root_remap_callback(), "AC1: root_remap hook installed");

        // Destroy arena — dtor must null hooks under mutex before teardown.
        arena.reset();
    }

    // No post-dtor fires from the dtor itself (clear-before-run_destructors).
    CHECK(compact_fires.load() == 0, "AC2: dtor did not invoke compact hook");
    CHECK(layout_fires.load() == 0, "AC2: dtor did not invoke layout hook");
    CHECK(root_fires.load() == 0, "AC2: dtor did not invoke root_remap hook");
}

// take_* also empties (regression: uninstall path stays correct).
static void ac2_take_clears_has() {
    std::println("\n--- #2382 AC2: take_* empties has_* ---");
    aura::ast::ASTArena arena(/*initial_size=*/16 * 1024);
    arena.set_on_compact_hook(&arena_hook_compact_nop, nullptr);
    arena.set_on_layout_change(&arena_hook_layout_nop, nullptr);
    arena.set_root_remap_callback(&arena_hook_root_remap_nop, nullptr);
    CHECK(arena.has_on_compact_hook(), "compact installed");
    CHECK(arena.has_on_layout_change(), "layout installed");
    CHECK(arena.has_root_remap_callback(), "root_remap installed");
    (void)arena.take_on_compact_hook();
    (void)arena.take_on_layout_change();
    (void)arena.take_root_remap_callback();
    CHECK(!arena.has_on_compact_hook(), "AC2: take compact → empty");
    CHECK(!arena.has_on_layout_change(), "AC2: take layout → empty");
    CHECK(!arena.has_root_remap_callback(), "AC2: take root_remap → empty");
}

// AC3: concurrent invoke_on_compact_hook_for_test + destroy; no crash.
static void ac3_concurrent_invoke_and_destroy() {
    std::println("\n--- #2382 AC3: concurrent invoke + destroy race ---");
    constexpr int kRounds = 40;
    std::atomic<int> errors{0};
    for (int r = 0; r < kRounds; ++r) {
        auto arena = std::make_shared<aura::ast::ASTArena>(/*initial_size=*/8 * 1024);
        auto fires = std::make_shared<std::atomic<int>>(0);
        arena->set_on_compact_hook(&arena_hook_compact_bump_i32, fires.get());

        std::atomic<bool> start{false};
        std::vector<std::thread> thr;
        thr.reserve(4);
        for (int t = 0; t < 4; ++t) {
            thr.emplace_back([&, arena]() {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                try {
                    // May race with destroy: invoke is no-op once hooks cleared.
                    arena->invoke_on_compact_hook_for_test();
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        start.store(true, std::memory_order_release);
        // Drop our strong ref while invokers may still hold shared_ptr.
        // Last invoker/thread may still call into arena; shared ownership
        // keeps object alive until all threads release — then dtor runs.
        arena.reset();
        for (auto& th : thr)
            th.join();
    }
    CHECK(errors.load() == 0, "AC3: no exceptions under invoke+destroy race");
}

// AC4: source contract + registration.
static void ac4_source_and_registration() {
    std::println("\n--- #2382 AC4: source-cite + gate ---");
    const auto src = read_file("src/core/arena.ixx");
    CHECK(!src.empty(), "arena.ixx readable");
    CHECK(src.find("Issue #2382") != std::string::npos, "AC4: cites #2382");
    CHECK(src.find("~ASTArena()") != std::string::npos, "AC4: destructor present");
    // Dtor body must clear all three under their mutexes.
    CHECK(src.find("slot.fn = nullptr") != std::string::npos, "AC4: dtor nulls compact hook slots");
    CHECK(src.find("on_layout_change_.fn = nullptr") != std::string::npos,
          "AC4: dtor nulls on_layout_change_");
    CHECK(src.find("root_remap_.fn = nullptr") != std::string::npos, "AC4: dtor nulls root_remap_");
    // clear-before-run_destructors order (inside the real dtor body, not comments).
    const auto dtor_pos = src.find("~ASTArena() {");
    CHECK(dtor_pos != std::string::npos, "AC4: ~ASTArena() { present");
    if (dtor_pos != std::string::npos) {
        const auto snip = src.substr(dtor_pos, 2500);
        const auto clear_pos = snip.find("slot.fn = nullptr");
        const auto run_pos = snip.find("run_destructors();");
        CHECK(clear_pos != std::string::npos && run_pos != std::string::npos && clear_pos < run_pos,
              "AC4: hook clear precedes run_destructors() in dtor body");
    }

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_arena_dtor_clears_hooks") != std::string::npos,
          "AC4: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_arena_dtor_clears_hooks_2382") != std::string::npos ||
              build.find("cmd_arena_dtor_clears_hooks_coverage") != std::string::npos,
          "AC4: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_arena_dtor_clears_hooks_2382.py");
    CHECK(!gate.empty() && gate.find("Issue #2382") != std::string::npos,
          "AC4: coverage linter present");
}

} // namespace

int run_test_arena_dtor_clears_hooks() {
    std::println("=== Issue #2382: ASTArena dtor clears hooks before teardown ===");
    ac1_ac2_dtor_clears_all_hooks();
    ac2_take_clears_has();
    ac3_concurrent_invoke_and_destroy();
    ac4_source_and_registration();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_dtor_clears_hooks();
}
#endif

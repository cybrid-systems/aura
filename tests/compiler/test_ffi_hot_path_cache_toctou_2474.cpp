// @category: unit
// @reason: Issue #2474 — FFI hot-path cache torn update closed
//          (sig_hash published last; invalidate-first; reader double-check).
//
//   AC1: N-thread dispatch_named with rotating sig_hashes — each call
//        returns expected fn result (no wrong-fn dispatch)
//   AC2: update_cache store order: hash invalidate → abi → fn → hash LAST
//   AC3: dispatch_batch + dispatch_cellgrid both double-check hash
//   AC4: clear_cache safe under concurrent dispatch
//   AC5: ffi_hot_path_cache_update_race_total + gate wiring

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;

namespace {

using aura::compiler::ffi_hot::ffi_sig_hash;
using aura::compiler::ffi_hot::FFIBatchHotPath;
using aura::compiler::ffi_hot::g_ffi_hot_path_stats;
using aura::compiler::ffi_hot::RenderFfiAbi;
using aura::compiler::ffi_hot::reset_ffi_hot_path_for_test;
using aura::renderer::DirtyRegion;
using aura::renderer::TermCell;
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

static int stress_iters() {
    // Default: 8 threads × 125000 = 1M dispatches.
    if (const char* e = std::getenv("AURA_2474_ITERS")) {
        const int v = std::atoi(e);
        if (v > 0)
            return v;
    }
    return 125000;
}

// Distinct batch backends: return a unique id so wrong-fn is detectable.
// tag is baked into each trampoline via template.
template <int Tag> static std::int64_t batch_fn_tag(const std::int64_t* args, std::size_t argc) {
    std::int64_t s = Tag * 1000;
    for (std::size_t i = 0; i < argc; ++i)
        s += args[i];
    return s;
}

static std::int64_t cellgrid_fn_tag42(const TermCell* /*cells*/, std::int32_t w, std::int32_t h,
                                      const DirtyRegion* /*d*/) {
    return 42 + static_cast<std::int64_t>(w) * 100 + static_cast<std::int64_t>(h);
}

static std::int64_t cellgrid_fn_tag99(const TermCell* /*cells*/, std::int32_t w, std::int32_t h,
                                      const DirtyRegion* /*d*/) {
    return 99 + static_cast<std::int64_t>(w) * 100 + static_cast<std::int64_t>(h);
}

// ── AC2: source store order ──
static void ac2_source_order() {
    std::println("\n--- #2474 AC2: update_cache store order ---");
    auto hh = read_file("src/compiler/ffi_hot_path.hh");
    CHECK(!hh.empty(), "AC2: read ffi_hot_path.hh");
    CHECK(hh.find("Issue #2474") != std::string::npos, "AC2: cites #2474");
    auto pos = hh.find("void update_cache");
    CHECK(pos != std::string::npos, "AC2: update_cache present");
    if (pos != std::string::npos) {
        auto win = hh.substr(pos, 900);
        // invalidate hash first
        CHECK(win.find("cached_sig_hash.store(0") != std::string::npos,
              "AC2: invalidate hash first");
        // hash LAST after fn
        auto fn_pos = win.find("cached_func_ptr.store");
        auto hash_last = win.rfind("cached_sig_hash.store");
        CHECK(fn_pos != std::string::npos && hash_last != std::string::npos && hash_last > fn_pos,
              "AC2: hash store after fn store");
        CHECK(win.find("hash LAST") != std::string::npos ||
                  win.find("sig_hash LAST") != std::string::npos ||
                  win.find("hash last") != std::string::npos,
              "AC2: documents hash LAST");
    }
    // Bug pattern: store hash first then fn (old order) must not remain
    // as the sole publish sequence without invalidate.
    CHECK(hh.find("ffi_hot_path_cache_update_race_total") != std::string::npos,
          "AC2: race counter present");
}

// ── AC3: double-check on both dispatch paths ──
static void ac3_double_check() {
    std::println("\n--- #2474 AC3: dispatch double-check ---");
    auto hh = read_file("src/compiler/ffi_hot_path.hh");
    auto batch = hh.find("dispatch_batch(");
    auto cell = hh.find("dispatch_cellgrid(");
    CHECK(batch != std::string::npos && cell != std::string::npos, "AC3: both dispatch methods");
    if (batch != std::string::npos) {
        auto win = hh.substr(batch, 1800);
        CHECK(win.find("h2") != std::string::npos ||
                  win.find("cached_sig_hash.load") != std::string::npos,
              "AC3: batch re-loads hash");
        CHECK(win.find("ffi_hot_path_cache_update_race_total") != std::string::npos,
              "AC3: batch bumps race counter");
    }
    if (cell != std::string::npos) {
        auto win = hh.substr(cell, 1800);
        CHECK(win.find("ffi_hot_path_cache_update_race_total") != std::string::npos,
              "AC3: cellgrid bumps race counter");
        CHECK(win.find("Issue #2474") != std::string::npos, "AC3: cellgrid cites #2474");
    }

    // Functional: rotating two batch fns on one cache — results match tag.
    reset_ffi_hot_path_for_test();
    FFIBatchHotPath hot;
    void* fn0 = reinterpret_cast<void*>(&batch_fn_tag<1>);
    void* fn1 = reinterpret_cast<void*>(&batch_fn_tag<2>);
    const auto h0 = ffi_sig_hash("bind0", "batch (I64*)");
    const auto h1 = ffi_sig_hash("bind1", "batch (I64*)");
    std::array<std::int64_t, 1> args{7};
    for (int i = 0; i < 200; ++i) {
        const bool use0 = (i % 2) == 0;
        const auto h = use0 ? h0 : h1;
        void* fn = use0 ? fn0 : fn1;
        const auto r = hot.dispatch_batch(h, fn, RenderFfiAbi::BatchArgs, args);
        const auto expect = use0 ? (1000 + 7) : (2000 + 7);
        CHECK(r == expect, "AC3: rotating batch returns matching tag");
        if (r != expect)
            break;
    }

    // CellGrid rotating
    TermCell grid_cell{};
    DirtyRegion dirty{};
    void* cg0 = reinterpret_cast<void*>(&cellgrid_fn_tag42);
    void* cg1 = reinterpret_cast<void*>(&cellgrid_fn_tag99);
    const auto ch0 = ffi_sig_hash("cg0", "cellgrid");
    const auto ch1 = ffi_sig_hash("cg1", "cellgrid");
    for (int i = 0; i < 100; ++i) {
        const bool use0 = (i % 2) == 0;
        const auto r = hot.dispatch_cellgrid(use0 ? ch0 : ch1, use0 ? cg0 : cg1, &grid_cell, 3, 4,
                                             &dirty, true);
        const auto expect = use0 ? (42 + 300 + 4) : (99 + 300 + 4);
        CHECK(r == expect, "AC3: rotating cellgrid returns matching tag");
        if (r != expect)
            break;
    }
}

// ── AC4: clear_cache under concurrent dispatch ──
static void ac4_clear_concurrent() {
    std::println("\n--- #2474 AC4: clear_cache concurrent ---");
    reset_ffi_hot_path_for_test();
    FFIBatchHotPath hot;
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
    std::atomic<int> done{0};
    void* fn = reinterpret_cast<void*>(&batch_fn_tag<5>);
    const auto h = ffi_sig_hash("clear", "batch");
    std::array<std::int64_t, 1> args{1};

    std::vector<std::thread> thr;
    for (int t = 0; t < 4; ++t) {
        thr.emplace_back([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 5000; ++i) {
                const auto r = hot.dispatch_batch(h, fn, RenderFfiAbi::BatchArgs, args);
                if (r != 5000 + 1 && r != -1 && r != 0)
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    thr.emplace_back([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < 2000; ++i)
            hot.clear_cache();
        done.fetch_add(1, std::memory_order_relaxed);
    });
    go.store(true, std::memory_order_release);
    for (auto& t : thr)
        t.join();
    CHECK(done.load() == 5, "AC4: all workers done");
    CHECK(bad.load() == 0, "AC4: no unexpected results under clear");
}

// ── AC1: multi-thread rotating hashes stress ──
static void ac1_stress() {
    std::println("\n--- #2474 AC1: multi-thread rotating sig_hash stress ---");
    reset_ffi_hot_path_for_test();
    // Shared hot path (one cache, many signatures rotating) — the race surface.
    FFIBatchHotPath hot;

    constexpr int kTags = 8;
    std::array<void*, kTags> fns{
        reinterpret_cast<void*>(&batch_fn_tag<10>), reinterpret_cast<void*>(&batch_fn_tag<11>),
        reinterpret_cast<void*>(&batch_fn_tag<12>), reinterpret_cast<void*>(&batch_fn_tag<13>),
        reinterpret_cast<void*>(&batch_fn_tag<14>), reinterpret_cast<void*>(&batch_fn_tag<15>),
        reinterpret_cast<void*>(&batch_fn_tag<16>), reinterpret_cast<void*>(&batch_fn_tag<17>),
    };
    std::array<std::uint64_t, kTags> hashes{};
    for (int i = 0; i < kTags; ++i)
        hashes[i] = ffi_sig_hash(std::format("bind{}", i), "batch (I64*)");

    const int kIters = stress_iters();
    constexpr int kThreads = 8;
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> mismatches{0};
    std::vector<std::thread> thr;
    thr.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        thr.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            std::array<std::int64_t, 1> args{static_cast<std::int64_t>(t + 1)};
            for (int i = 0; i < kIters; ++i) {
                const int tag = (t + i) % kTags;
                const auto r =
                    hot.dispatch_batch(hashes[tag], fns[tag], RenderFfiAbi::BatchArgs, args);
                // Expected: Tag*1000 + arg  (batch_fn_tag)
                const auto expect = static_cast<std::int64_t>((10 + tag) * 1000) + args[0];
                if (r != expect)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : thr)
        th.join();

    const auto total = ops.load();
    const auto bad = mismatches.load();
    const auto races = g_ffi_hot_path_stats().ffi_hot_path_cache_update_race_total.load();
    std::println("  ops={} mismatches={} race_detects={} iters/thread={}", total, bad, races,
                 kIters);
    CHECK(total == static_cast<std::uint64_t>(kIters) * kThreads, "AC1: full op count");
    CHECK(bad == 0, "AC1: zero wrong-fn dispatches");
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2474 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/check_ffi_hot_path_cache_toctou_2474.py");
    CHECK(build.find("check_ffi_hot_path_cache_toctou_2474") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_ffi_hot_path_cache_toctou_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_ffi_hot_path_cache_toctou_2474") != std::string::npos,
          "AC5: cmake test");
    CHECK(!script.empty() && script.find("2474") != std::string::npos, "AC5: check script exists");
}

// Baseline hit/miss still works
static void ac_baseline() {
    std::println("\n--- #2474 baseline hit/miss ---");
    reset_ffi_hot_path_for_test();
    FFIBatchHotPath hot;
    void* fn = reinterpret_cast<void*>(&batch_fn_tag<3>);
    const auto h = ffi_sig_hash("base", "batch");
    std::array<std::int64_t, 2> args{1, 2};
    const auto r0 = hot.dispatch_batch(h, fn, RenderFfiAbi::BatchArgs, args);
    CHECK(r0 == 3000 + 1 + 2, "baseline miss result");
    const auto r1 = hot.dispatch_batch(h, fn, RenderFfiAbi::BatchArgs, args);
    CHECK(r1 == 3000 + 1 + 2, "baseline hit result");
    CHECK(g_ffi_hot_path_stats().hit_total.load() >= 1, "baseline had hit");
}

} // namespace

int main() {
    std::println("=== Issue #2474: FFI hot-path cache TOCTOU ===");
    ac2_source_order();
    ac_baseline();
    ac3_double_check();
    ac4_clear_concurrent();
    ac1_stress();
    ac5_gate();
    std::println("\n=== #2474 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

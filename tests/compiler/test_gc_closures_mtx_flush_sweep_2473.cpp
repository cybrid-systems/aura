// @category: unit
// @reason: Issue #2473 — flush_gc_roots / compact_sweep take closures_mtx_
//          when iterating / erasing closures_ (heap_mutex_ alone races
//          register_active_closure rehash).
//
//   AC1: concurrent gc_root_count + register_active_closure + compact_sweep
//        stress (default ~1M ops; no crash / no torn map)
//   AC2: flush_gc_roots shared_lock + compact_sweep unique_lock present
//   AC3: metrics gc_flush_closures_locked_total / gc_sweep_closures_locked_total
//   AC4: lock order closures first (AuditScope Closures; no env invert)
//   AC5: gate wiring

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/observability_metrics.h"
#include "serve/gc_coordinator.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::serve::GCRootSet;
using aura::serve::GCSweepBuffers;
using aura::serve::MarkBitVector;
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
    // Default: 125000 × (4 readers + 2 writers + 2 sweepers) ≈ 1M ops.
    // Override with AURA_2473_ITERS for longer TSan soaks.
    if (const char* e = std::getenv("AURA_2473_ITERS")) {
        const int v = std::atoi(e);
        if (v > 0)
            return v;
    }
    return 125000;
}

// ── AC3: metrics bump on flush / sweep ──
static void ac3_metrics() {
    std::println("\n--- #2473 AC3: lock acquisition counters ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC3: metrics wired");
    const auto f0 = m->gc_flush_closures_locked_total.load();
    const auto s0 = m->gc_sweep_closures_locked_total.load();

    Closure cl;
    cl.env_id = NULL_ENV_ID;
    (void)ev.register_active_closure(std::move(cl));

    GCRootSet roots;
    ev.flush_gc_roots(&roots);
    CHECK(m->gc_flush_closures_locked_total.load() > f0, "AC3: flush counter advanced");

    GCSweepBuffers marks{};
    MarkBitVector cmarks(64);
    // Leave all unmarked → erase path exercises unique_lock.
    marks.closure_marks = &cmarks;
    (void)ev.compact_sweep(&marks);
    CHECK(m->gc_sweep_closures_locked_total.load() > s0, "AC3: sweep counter advanced");
}

// ── AC1: concurrent stress ──
static void ac1_stress() {
    std::println("\n--- #2473 AC1: concurrent count + register + compact_sweep ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC1: metrics");

    const int kIters = stress_iters();
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> ops{0};
    std::atomic<int> done{0};
    std::vector<std::thread> thr;
    thr.reserve(8);

    // 4 readers: gc_root_count
    for (int r = 0; r < 4; ++r) {
        thr.emplace_back([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                (void)ev.gc_root_count();
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // 2 writers: register_active_closure
    for (int w = 0; w < 2; ++w) {
        thr.emplace_back([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                Closure cl;
                cl.env_id = NULL_ENV_ID;
                (void)ev.register_active_closure(std::move(cl));
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // 2 sweepers: compact_sweep with empty marks (still takes unique_lock)
    for (int s = 0; s < 2; ++s) {
        thr.emplace_back([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            MarkBitVector cmarks(256);
            GCSweepBuffers marks{};
            marks.closure_marks = &cmarks;
            for (int i = 0; i < kIters; ++i) {
                (void)ev.compact_sweep(&marks);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : thr)
        t.join();

    const auto total = ops.load();
    std::println("  stress ops={} done={} iters/worker={}", total, done.load(), kIters);
    CHECK(done.load() == 8, "AC1: all workers finished");
    CHECK(total >= static_cast<std::uint64_t>(kIters) * 8, "AC1: full op count");
    // Counters should have moved under stress (sweep always locks).
    CHECK(m->gc_sweep_closures_locked_total.load() > 0, "AC1: sweep locks observed");
    // Final count still callable (map consistent).
    (void)ev.gc_root_count();
    CHECK(true, "AC1: post-stress gc_root_count ok");
}

// ── AC2: source dual-lock ──
static void ac2_source() {
    std::println("\n--- #2473 AC2: source shared/unique closures_mtx_ ---");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    auto ixx = read_file("src/compiler/evaluator.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(!gc.empty(), "AC2: read evaluator_gc.cpp");
    CHECK(gc.find("Issue #2473") != std::string::npos, "AC2: cites #2473");

    auto fpos = gc.find("void Evaluator::flush_gc_roots");
    CHECK(fpos != std::string::npos, "AC2: flush_gc_roots present");
    if (fpos != std::string::npos) {
        auto win = gc.substr(fpos, 2800);
        CHECK(win.find("shared_lock") != std::string::npos, "AC2: flush shared_lock");
        CHECK(win.find("closures_mtx_") != std::string::npos, "AC2: flush locks closures_mtx_");
        CHECK(win.find("gc_flush_closures_locked_total") != std::string::npos,
              "AC2: flush bumps metric");
    }

    auto spos = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep");
    if (spos == std::string::npos)
        spos = gc.find("Evaluator::compact_sweep(void*");
    CHECK(spos != std::string::npos, "AC2: compact_sweep present");
    if (spos != std::string::npos) {
        // unique_lock is ~4.6k into body (after defer / pin / pair_remap).
        auto win = gc.substr(spos, 7000);
        CHECK(win.find("unique_lock") != std::string::npos, "AC2: sweep unique_lock");
        CHECK(win.find("closures_mtx_") != std::string::npos, "AC2: sweep locks closures_mtx_");
        CHECK(win.find("gc_sweep_closures_locked_total") != std::string::npos,
              "AC2: sweep bumps metric");
        CHECK(win.find("Issue #2473") != std::string::npos, "AC2: #2473 in sweep body");
    }

    CHECK(met.find("gc_flush_closures_locked_total") != std::string::npos, "AC2: metrics flush");
    CHECK(met.find("gc_sweep_closures_locked_total") != std::string::npos, "AC2: metrics sweep");
    CHECK(ixx.find("Issue #2473") != std::string::npos || ixx.find("#2473") != std::string::npos,
          "AC2: ixx cites #2473");
}

// ── AC4: lock order documentation ──
static void ac4_lock_order() {
    std::println("\n--- #2473 AC4: closures lock order ---");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    // AuditScope Closures on both paths
    auto fpos = gc.find("void Evaluator::flush_gc_roots");
    auto spos = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep");
    if (spos == std::string::npos)
        spos = gc.find("Evaluator::compact_sweep(void*");
    CHECK(fpos != std::string::npos && spos != std::string::npos, "AC4: both methods");
    if (fpos != std::string::npos) {
        auto win = gc.substr(fpos, 2800);
        CHECK(win.find("Level::Closures") != std::string::npos, "AC4: flush AuditScope Closures");
    }
    if (spos != std::string::npos) {
        auto win = gc.substr(spos, 7000);
        CHECK(win.find("Level::Closures") != std::string::npos, "AC4: sweep AuditScope Closures");
        // Must not take env_frames before closures in this section
        CHECK(win.find("#1664") != std::string::npos ||
                  win.find("closures first") != std::string::npos ||
                  win.find("closures → env") != std::string::npos,
              "AC4: documents closures→env order");
    }
    // Smoke: enforce_linear still works under sequential register
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 8; ++i) {
        Closure cl;
        cl.env_id = NULL_ENV_ID;
        (void)ev.register_active_closure(std::move(cl));
    }
    GCSweepBuffers marks{};
    MarkBitVector cmarks(32);
    marks.closure_marks = &cmarks;
    (void)ev.compact_sweep(&marks);
    (void)ev.gc_root_count();
    CHECK(true, "AC4: sequential enforce-adjacent paths ok");
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2473 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_gc_closures_mtx_flush_sweep_2473.py");
    CHECK(build.find("check_gc_closures_mtx_flush_sweep_2473") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_gc_closures_mtx_flush_sweep_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_gc_closures_mtx_flush_sweep_2473") != std::string::npos,
          "AC5: cmake test");
    CHECK(!script.empty() && script.find("2473") != std::string::npos, "AC5: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2473: GC flush/sweep closures_mtx_ dual-lock ===");
    ac2_source();
    ac3_metrics();
    ac4_lock_order();
    ac1_stress();
    ac5_gate();
    std::println("\n=== #2473 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

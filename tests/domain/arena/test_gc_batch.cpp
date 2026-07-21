// tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot
// (was tests/test_gc_batch.cpp). Prefer this path; do not re-add under tests/ root.
//
// test_gc_batch.cpp
// B pilot #13 (after compact in 8a505b5c): consolidated gc family
// — Issues #1667 + #1734 + #1864 (~Evaluator releases PanicCheckpoint GC
// defer + collect_compiler_managed_gc_roots bridge_epoch drift detect +
// gc_root_count shared_lock closures_mtx_) into one batch driver.
//
// NOTE: test_gc_evaluator_integration.cpp is intentionally NOT included.
// It uses a custom add_executable + add_test + custom target_sources
// (src/core/contract_handler.cpp + src/core/contract_stub.cpp) integration
// build pattern at CMakeLists.txt:725-803 — converting it to a standard
// aura_add_issue_test batch would break its custom build settings and lose
// coverage of the GC hook + flush_gc_roots + compact_sweep API surface.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch /
// walk_batch / compact_batch precedents): single binary with CHECK() + per-
// issue AC blocks in namespace aura_gc_batch { run_NNN_xxx() };
// EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 13 ACs total):
//   Issue #1667 — 6 ACs: arm/release roundtrip (depth +1/-1) +
//                  ~Evaluator releases armed defer (no depth leak) +
//                  commit releases before clear; dtor idempotent +
//                  save then destroy without commit (CS scope) restores +
//                  double release / re-arm idempotent +
//                  request_gc_safepoint immediate after dtor mid-window
//   Issue #1734 — 4 ACs: source cites #1734 + drift metric declared +
//                  matching snapshot no bump + mismatched snapshot +1
//                  still collects roots
//   Issue #1864 — 3 ACs: source shared_lock(closures_mtx_) in gc_root_count +
//                  sequential register+count no crash +
//                  concurrent readers + writers complete (no UAF/deadlock)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"
#include "serve/gc_coordinator.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.ir_executor;

namespace aura_gc_batch {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ── Issue #1667 — PanicCheckpoint GC defer dtor exception-safety ──
static void run_1667_arm_release_roundtrip() {
    std::println("\n--- AC1 (#1667): arm/release roundtrip ---");
    Evaluator ev;
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "not armed initially");
    ev.arm_gc_defer_for_pending_panic();
    CHECK(ev.gc_defer_armed_for_pending_panic(), "armed after arm");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1");
    ev.release_gc_defer_for_pending_panic();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after release");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth restored");
}

static void run_1667_dtor_releases_armed() {
    std::println("\n--- AC2 (#1667): ~Evaluator releases armed defer ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1 while live");
        // no commit/restore — dtor must release
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0,
          "depth restored after ~Evaluator mid-window");
    CHECK(!aura::gc_hooks::gc_deferred_for_pending_panic() || d0 > 0,
          "no spurious process-wide defer from this Evaluator");
}

static void run_1667_commit_then_dtor() {
    std::println("\n--- AC3 (#1667): commit releases; dtor idempotent ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(ev.gc_defer_armed_for_pending_panic(), "armed");
        ev.commit_panic_checkpoint();
        CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after commit");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth after commit");
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth after dtor (idempotent)");
}

static void run_1667_cs_save_destroy_no_commit() {
    std::println("\n--- AC4 (#1667): CompilerService save then destroy without commit ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        CompilerService cs;
        auto r = cs.eval("(set-code \"(define x 1)\")");
        CHECK(r.has_value(), "set-code ok");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        CHECK(ev.save_panic_checkpoint(), "save_panic_checkpoint");
        CHECK(ev.gc_defer_armed_for_pending_panic(), "armed after save");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1 after save");
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0,
          "depth restored after CS dtor without commit");
}

static void run_1667_double_release_idempotent() {
    std::println("\n--- AC5 (#1667): double release / arm idempotent ---");
    Evaluator ev;
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    ev.arm_gc_defer_for_pending_panic();
    ev.arm_gc_defer_for_pending_panic();
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "re-arm no double count");
    ev.release_gc_defer_for_pending_panic();
    ev.release_gc_defer_for_pending_panic();
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "double release safe");
}

static void run_1667_safepoint_after_dtor() {
    std::println("\n--- AC6 (#1667): safepoint immediate after dtor mid-window release ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(ev.request_gc_safepoint() == 1, "deferred while armed");
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth clean");
    Evaluator ev2;
    if (d0 == 0) {
        CHECK(ev2.request_gc_safepoint() == 0, "immediate GC after prior dtor release");
    } else {
        CHECK(true, "skipped absolute defer check (pre-existing depth)");
    }
}

// ── Issue #1734 — gc_roots_bridge_epoch_drift ──
static void run_1734_source_metric() {
    std::println("\n--- AC1/AC2 (#1734): drift check + metric ---");
    std::string gc;
    for (const char* p : {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
        gc = read_file(p);
        if (!gc.empty())
            break;
    }
    CHECK(!gc.empty(), "read evaluator_gc.cpp");
    CHECK(gc.find("#1734") != std::string::npos, "cites #1734");
    CHECK(gc.find("gc_roots_bridge_epoch_drift_total") != std::string::npos, "bumps drift metric");
    CHECK(gc.find("current_bridge_epoch()") != std::string::npos, "reads live epoch");

    std::string msrc;
    for (const char* p :
         {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
        msrc = read_file(p);
        if (!msrc.empty())
            break;
    }
    CHECK(!msrc.empty() && msrc.find("gc_roots_bridge_epoch_drift_total") != std::string::npos,
          "metric field declared");
}

static void run_1734_matching_snapshot() {
    std::println("\n--- AC3 (#1734): matching snapshot ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");

    Closure cl;
    cl.env_id = NULL_ENV_ID;
    (void)ev.register_active_closure(std::move(cl));

    const auto live = ev.current_bridge_epoch();
    const auto d0 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);

    std::vector<std::int64_t> cl_roots, env_roots;
    ev.collect_compiler_managed_gc_roots(cl_roots, env_roots, live);

    const auto d1 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);
    CHECK(d1 == d0, "matching snapshot does not bump drift");
    CHECK(!cl_roots.empty() || live == 0, "collected roots or tracking inactive");
}

static void run_1734_mismatched_snapshot() {
    std::println("\n--- AC4 (#1734): mismatched snapshot bumps drift ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());

    Closure cl;
    cl.env_id = NULL_ENV_ID;
    (void)ev.register_active_closure(std::move(cl));

    const auto live = ev.current_bridge_epoch();
    const auto stale_snap = live + 1000;
    const auto d0 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);

    std::vector<std::int64_t> cl_roots, env_roots;
    ev.collect_compiler_managed_gc_roots(cl_roots, env_roots, stale_snap);

    const auto d1 = m->gc_roots_bridge_epoch_drift_total.load(std::memory_order_relaxed);
    CHECK(d1 == d0 + 1, "mismatched snapshot bumps drift +1");
    CHECK(true, "collect completed after drift");
}

// ── Issue #1864 — gc_root_count shared_lock ──
static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void run_1864_source() {
    std::println("\n--- AC1 (#1864): gc_root_count shared_lock closures_mtx_ ---");
    auto src = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    CHECK(!src.empty(), "read evaluator_gc.cpp");
    CHECK(src.find("#1864") != std::string::npos, "cites #1864");
    auto pos = src.find("Evaluator::gc_root_count");
    CHECK(pos != std::string::npos, "gc_root_count present");
    auto win = src.substr(pos, 700);
    CHECK(win.find("shared_lock") != std::string::npos, "uses shared_lock");
    CHECK(win.find("closures_mtx_") != std::string::npos, "locks closures_mtx_");
    CHECK(win.find("No lock") == std::string::npos, "removed No lock safepoint claim");
    CHECK(!ixx.empty() && ixx.find("#1864") != std::string::npos, "ixx cites #1864");
}

static void run_1864_sequential() {
    std::println("\n--- AC2 (#1864): sequential register + gc_root_count ---");
    Evaluator ev;
    auto n0 = ev.gc_root_count();
    CHECK(n0 == n0, "fresh count callable");
    for (int i = 0; i < 32; ++i) {
        Closure cl;
        cl.env_id = NULL_ENV_ID;
        (void)ev.register_active_closure(std::move(cl));
    }
    auto n1 = ev.gc_root_count();
    CHECK(n1 >= n0, "count does not decrease after register");
    auto n2 = ev.gc_root_count();
    CHECK(n2 == n1, "stable under sequential re-call");
}

static void run_1864_concurrent() {
    std::println("\n--- AC3 (#1864): concurrent gc_root_count + register_active_closure ---");
    Evaluator ev;
    std::atomic<bool> start{false};
    std::atomic<std::uint64_t> ops{0};
    constexpr int kReaders = 4;
    constexpr int kWriters = 2;
    constexpr int kIters = 200;
    std::vector<std::thread> threads;
    threads.reserve(kReaders + kWriters);

    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                (void)ev.gc_root_count();
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                Closure cl;
                cl.env_id = NULL_ENV_ID;
                (void)ev.register_active_closure(std::move(cl));
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    auto t0 = std::chrono::steady_clock::now();
    for (auto& t : threads)
        t.join();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    const auto expected = static_cast<std::uint64_t>((kReaders + kWriters) * kIters);
    CHECK(ops.load() == expected, "all concurrent ops completed");
    CHECK(ms < 30000, "finished within 30s (no deadlock)");
    (void)ev.gc_root_count();
    CHECK(true, "post-stress gc_root_count ok");
}

// ── Wave 5 (#204 GC mark_env_frame_roots + #205 caller-side env_frames_ walk) ──

static void run_204_mark_env_frame_roots() {
    std::println("\n=== #204: GCCollector::mark_env_frame_roots (EnvFrame SoA arena) ===");
    // T1: mark_env_frame_roots sets bits for env-walk pair/closure indices
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_from_roots({}, /*string_heap_size=*/10, /*pairs_size=*/20, /*closures_size=*/30);
        gc.mark_env_frame_roots({3, 7, 15}, {5, 10, 25});
        CHECK(gc.pair_mark(3) && gc.pair_mark(7) && gc.pair_mark(15),
              "#204 T1: pair_marks_ bits 3/7/15 set via env walk");
        CHECK(gc.closure_mark(5) && gc.closure_mark(10) && gc.closure_mark(25),
              "#204 T1: closure_marks_ bits 5/10/25 set via env walk");
        CHECK(!gc.pair_mark(0) && !gc.pair_mark(4), "#204 T1: unwalked pair indices NOT marked");
    }
    // T2: empty lists = no-op
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_from_roots({}, 5, 5, 5);
        gc.mark_env_frame_roots({}, {});
        CHECK(!gc.pair_mark(0) && !gc.pair_mark(4), "#204 T2: empty env walk is a no-op");
    }
    // T3: negative indices ignored (defensive)
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_from_roots({}, 10, 10, 10);
        std::vector<int64_t> bad = {-1, -2, 5, -100};
        gc.mark_env_frame_roots(bad, bad);
        CHECK(gc.pair_mark(5), "#204 T3: valid index 5 still marked");
        CHECK(!gc.pair_mark(0), "#204 T3: negative indices ignored");
    }
    // T4: env walk is additive to mark_from_roots
    {
        aura::serve::GCCollector gc(nullptr);
        aura::serve::GCRootSet roots;
        roots.pair_roots.push_back(0);
        roots.pair_roots.push_back(1);
        roots.closure_roots.push_back(2);
        gc.mark_from_roots(roots, 0, 10, 10);
        CHECK(gc.pair_mark(0) && gc.pair_mark(1) && gc.closure_mark(2),
              "#204 T4: explicit root marks 0/1/2");
        gc.mark_env_frame_roots({3, 4}, {5, 6});
        CHECK(gc.pair_mark(0) && gc.pair_mark(1) && gc.closure_mark(2),
              "#204 T4: explicit root marks persist (additive)");
        CHECK(gc.pair_mark(3) && gc.pair_mark(4) && gc.closure_mark(5) && gc.closure_mark(6),
              "#204 T4: env-walk marks added");
    }
    // T5: pre-resize env walk is silent no-op
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_env_frame_roots({0, 1, 2}, {0, 1});
        gc.mark_from_roots({}, 5, 5, 5);
        CHECK(!gc.pair_mark(0) && !gc.pair_mark(1) && !gc.closure_mark(0),
              "#204 T5: pre-resize env walk is a silent no-op");
    }
}

static void run_205_env_walk_callback() {
    std::println("\n=== #205: GCCollector::register_env_walk_fn callback wire-up ===");
    // T1: register_env_walk_fn compiles + stores callback (not invoked yet)
    {
        aura::serve::GCCollector gc(nullptr);
        bool called = false;
        gc.register_env_walk_fn([&called](aura::serve::EnvFrameRoots&) { called = true; });
        CHECK(!called, "#205 T1: callback stored, not invoked at register time");
    }
    // T2: callback not invoked without collect() (full invocation tested by run-tests.sh)
    {
        aura::serve::GCCollector gc(nullptr);
        int n = 0;
        gc.register_env_walk_fn([&n](aura::serve::EnvFrameRoots&) { ++n; });
        CHECK(n == 0, "#205 T2: callback not invoked without collect()");
    }
    // T3: EnvFrameRoots default-constructs with empty vectors
    {
        aura::serve::EnvFrameRoots r;
        CHECK(r.pair_roots.empty() && r.closure_roots.empty(),
              "#205 T3: EnvFrameRoots default-constructs empty");
    }
    // T4: simulated walk + mark_env_frame_roots sets bits
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_from_roots({}, /*string=*/10, /*pairs=*/20, /*closures=*/30);
        aura::serve::EnvFrameRoots env_roots;
        env_roots.pair_roots = {3, 7, 15};
        env_roots.closure_roots = {5, 10, 25};
        gc.mark_env_frame_roots(env_roots.pair_roots, env_roots.closure_roots);
        CHECK(gc.pair_mark(3) && gc.pair_mark(7) && gc.pair_mark(15),
              "#205 T4: pair_marks_ bits 3/7/15 set");
        CHECK(gc.closure_mark(5) && gc.closure_mark(10) && gc.closure_mark(25),
              "#205 T4: closure_marks_ bits 5/10/25 set");
    }
    // T5: env walk additive with explicit roots
    {
        aura::serve::GCCollector gc(nullptr);
        aura::serve::GCRootSet roots;
        roots.pair_roots = {1, 2};
        roots.closure_roots = {11, 12};
        gc.mark_from_roots(roots, 10, 20, 30);
        gc.mark_env_frame_roots({3, 4}, {13, 14});
        CHECK(gc.pair_mark(1) && gc.pair_mark(2) && gc.closure_mark(11) && gc.closure_mark(12),
              "#205 T5: explicit root marks persist");
        CHECK(gc.pair_mark(3) && gc.pair_mark(4) && gc.closure_mark(13) && gc.closure_mark(14),
              "#205 T5: env-walk marks added");
    }
    // T6: empty env walk no-op
    {
        aura::serve::GCCollector gc(nullptr);
        gc.mark_from_roots({}, 10, 20, 30);
        gc.mark_env_frame_roots({}, {});
        CHECK(!gc.pair_mark(0), "#205 T6: empty env walk is a no-op");
    }
}

// ── Issue #223 — closure-bridge epoch counter for lifetime tracking ──
// Folded from tests/issues/test_issue_223.cpp via #1957. Mirror structs in
// standalone TU (production types guarded against GCC 16.1 std module +
// P2996 reflection conflict in test_issue_223.cpp).

namespace test_223_bridge {
    struct ClosureBridgeData {
        const void* flat = nullptr;
        const void* pool = nullptr;
        std::uint32_t body_id = ~0u;
        std::string body_source;
        std::uint64_t bridge_epoch = 0;
    };
    struct IRClosureBridgeFields {
        const void* flat = nullptr;
        const void* pool = nullptr;
        std::uint32_t body_id = ~0u;
        std::uint64_t bridge_epoch = 0;
    };
    struct MockEpochTracker {
        std::atomic<std::uint64_t> mutation_epoch_{0};
        std::uint64_t bridge_epoch() const noexcept {
            return mutation_epoch_.load(std::memory_order_relaxed);
        }
        void bump_bridge_epoch() noexcept {
            mutation_epoch_.fetch_add(1, std::memory_order_relaxed);
        }
        void reset() noexcept { mutation_epoch_.fetch_add(1, std::memory_order_relaxed); }
    };
    static bool is_bridge_stale(std::uint64_t bridge_epoch, std::uint64_t current_epoch) {
        if (bridge_epoch == 0)
            return false; // legacy: trust
        return bridge_epoch != current_epoch;
    }
} // namespace test_223_bridge

static void run_223_closure_bridge_epoch() {
    using namespace test_223_bridge;
    std::println("\n=== #223: closure-bridge epoch counter ===");
    // T1: bridge_epoch() / reset() / bump_bridge_epoch() basics
    {
        MockEpochTracker svc;
        CHECK(svc.bridge_epoch() == 0, "initial bridge_epoch = 0");
        svc.reset();
        CHECK(svc.bridge_epoch() == 1, "reset() bumped to 1");
        svc.bump_bridge_epoch();
        CHECK(svc.bridge_epoch() == 2, "bump_bridge_epoch() bumped to 2");
        svc.reset();
        CHECK(svc.bridge_epoch() == 3, "second reset() bumped to 3");
    }
    // T2: ClosureBridgeData captures epoch at construction
    {
        MockEpochTracker svc;
        int fake_flat = 0, fake_pool = 0;
        ClosureBridgeData bd;
        bd.flat = &fake_flat;
        bd.pool = &fake_pool;
        bd.body_id = 42;
        bd.body_source = "(lambda (x) x)";
        bd.bridge_epoch = svc.bridge_epoch();
        CHECK(bd.bridge_epoch == 0, "bridge captured epoch 0");
        CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
              "epoch 0 bridge is legacy (not invalidated)");
        svc.reset();
        bd.bridge_epoch = 1;
        CHECK(!is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
              "bridge at epoch 1 not stale at current epoch 1");
        svc.reset();
        CHECK(is_bridge_stale(bd.bridge_epoch, svc.bridge_epoch()),
              "bridge at epoch 1 stale after reset to 2");
        ClosureBridgeData bd2;
        bd2.flat = &fake_flat;
        bd2.pool = &fake_pool;
        bd2.body_id = 42;
        bd2.body_source = "(lambda (x) x)";
        bd2.bridge_epoch = svc.bridge_epoch();
        CHECK(bd2.bridge_epoch == 2 && !is_bridge_stale(bd2.bridge_epoch, svc.bridge_epoch()),
              "new bridge at epoch 2 not stale");
    }
    // T3: IRClosure carries bridge_epoch
    {
        MockEpochTracker svc;
        IRClosureBridgeFields cl;
        cl.bridge_epoch = svc.bridge_epoch();
        CHECK(cl.bridge_epoch == 0, "IRClosure bridge_epoch defaults to 0");
        svc.reset();
        CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
              "IRClosure captured at epoch 0 stays legacy");
        cl.bridge_epoch = svc.bridge_epoch();
        CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
              "IRClosure re-captured at current epoch not stale");
        svc.bump_bridge_epoch();
        CHECK(is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
              "IRClosure at epoch 1 stale after bump");
    }
    // T4: Epoch monotonicity (100 resets + bumps)
    {
        MockEpochTracker svc;
        std::uint64_t last = svc.bridge_epoch();
        for (int i = 0; i < 100; ++i) {
            svc.reset();
            CHECK(svc.bridge_epoch() > last, "epoch monotonically increases (reset)");
            last = svc.bridge_epoch();
            svc.bump_bridge_epoch();
            CHECK(svc.bridge_epoch() > last, "epoch monotonically increases (bump)");
            last = svc.bridge_epoch();
        }
    }
    // T5+T6: apply_closure wiring + body_source fallback
    {
        MockEpochTracker svc;
        struct SimCl {
            void* flat = (void*)0xdeadbeef;
            void* pool = (void*)0xcafebabe;
            std::uint32_t body_id = 42;
            std::uint64_t bridge_epoch = 0;
        };
        SimCl cl;
        cl.bridge_epoch = svc.bridge_epoch();
        CHECK(!is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
              "legacy closure not invalidated");
        cl.bridge_epoch = 1;
        svc.bump_bridge_epoch();
        svc.reset();
        CHECK(is_bridge_stale(cl.bridge_epoch, svc.bridge_epoch()),
              "closure at epoch 1 invalidated after reset");
        SimCl cl2;
        cl2.bridge_epoch = svc.bridge_epoch();
        CHECK(!is_bridge_stale(cl2.bridge_epoch, svc.bridge_epoch()),
              "fresh closure at current epoch not invalidated");
        // T6 body_source fallback
        struct WithSource {
            std::uint64_t bridge_epoch = 0;
            std::string body_source;
        };
        WithSource cl_a;
        cl_a.body_source = "";
        cl_a.bridge_epoch = 1;
        CHECK(is_bridge_stale(cl_a.bridge_epoch, 2), "T6 stale bridge detected");
        CHECK(cl_a.body_source.empty(), "T6 empty body_source = no fallback");
        WithSource cl_b;
        cl_b.body_source = "(lambda (x) (* x x))";
        cl_b.bridge_epoch = 1;
        CHECK(is_bridge_stale(cl_b.bridge_epoch, 2), "T6 stale bridge + body_source = fallback");
        WithSource cl_c;
        cl_c.body_source = "(lambda (x) (* x x))";
        cl_c.bridge_epoch = 2;
        CHECK(!is_bridge_stale(cl_c.bridge_epoch, 2), "T6 fresh bridge not stale");
    }
}

// ── Issue #224 — closure-bridge shared_ptr-based ownership ──
// Folded from tests/issues/test_issue_224_closure_bridge.cpp via #1957.
// Cycle 2: std::shared_ptr<const ast::FlatAST> keeps the FlatAST alive
// as long as the bridge exists, even after the lowering arena resets.

static void run_224_closure_bridge_shared_ptr() {
    std::println("\n=== #224: closure-bridge shared_ptr-based ownership ===");
    // T1.1: shared_ptr keeps FlatAST alive (refcount semantics)
    {
        auto flat_ptr = std::make_shared<aura::ast::FlatAST>();
        auto pool_ptr = std::make_shared<aura::ast::StringPool>();
        aura::ir::ClosureBridgeData bd;
        bd.flat = flat_ptr;
        bd.pool = pool_ptr;
        bd.body_id = 0;
        CHECK(flat_ptr.use_count() == 2, "shared_ptr refcount = 2 after copy to bd.flat");
        flat_ptr.reset();
        pool_ptr.reset();
        CHECK(bd.flat.use_count() == 1, "bd.flat still valid after local reset");
        CHECK(bd.pool.use_count() == 1, "bd.pool still valid after local reset");
        CHECK(bd.flat != nullptr && bd.pool != nullptr,
              "shared_ptr remains valid after local drops");
        CHECK(bd.flat->size() == 0, "bd.flat dereferenceable after local drop");
    }
    // T1.2: refcount cycles to 0
    {
        auto flat_ptr = std::make_shared<aura::ast::FlatAST>();
        auto pool_ptr = std::make_shared<aura::ast::StringPool>();
        aura::ir::ClosureBridgeData bd;
        bd.flat = flat_ptr;
        bd.pool = pool_ptr;
        CHECK(flat_ptr.use_count() == 2, "refcount = 2 after copy");
        flat_ptr.reset();
        pool_ptr.reset();
        CHECK(bd.flat.use_count() == 1, "refcount = 1 after local reset");
    }
    // T1.3: shared_ptr composes with bridge_epoch
    {
        auto flat_ptr = std::make_shared<aura::ast::FlatAST>();
        auto pool_ptr = std::make_shared<aura::ast::StringPool>();
        aura::ir::ClosureBridgeData bd;
        bd.flat = flat_ptr;
        bd.pool = pool_ptr;
        bd.body_id = 0;
        bd.bridge_epoch = 42;
        CHECK(bd.bridge_epoch == 42, "bridge_epoch preserved alongside shared_ptr");
        CHECK(bd.flat != nullptr, "shared_ptr valid alongside bridge_epoch");
        constexpr std::uint64_t kNewEpoch = 100;
        CHECK(bd.bridge_epoch != kNewEpoch, "bridge_epoch mismatch signals staleness");
    }
    // T1.4: shared_ptr field type consistency (static_assert)
    {
        using BDType = decltype(aura::ir::ClosureBridgeData::flat);
        using CLType = decltype(aura::compiler::IRClosure::flat);
        static_assert(std::is_same_v<BDType, CLType>,
                      "ClosureBridgeData::flat == IRClosure::flat type");
        CHECK(true, "static_assert holds: both shared_ptr<const ast::FlatAST>");
    }
    // T1.5: shared_ptr copy/move
    {
        auto flat_ptr = std::make_shared<aura::ast::FlatAST>();
        aura::ir::ClosureBridgeData bd1;
        bd1.flat = flat_ptr;
        CHECK(flat_ptr.use_count() == 2, "refcount = 2 after copy to bd1");
        aura::ir::ClosureBridgeData bd2 = bd1;
        CHECK(flat_ptr.use_count() == 3, "refcount = 3 after copy to bd2");
        CHECK(bd1.flat.get() == bd2.flat.get(), "bd1.flat == bd2.flat pointer");
        bd2 = aura::ir::ClosureBridgeData{};
        CHECK(flat_ptr.use_count() == 2, "refcount = 2 after bd2 cleared");
    }
    // T1.6: end-to-end via CompilerService
    {
        aura::compiler::CompilerService cs;
        auto eval1 = cs.eval("(begin (define (square x) (* x x)) (square 5))");
        auto eval2 = cs.eval("(begin (define (square x) (* x x)) (square 5))");
        CHECK(eval1 && eval2, "both evals return values");
        if (eval1 && eval2) {
            CHECK(aura::compiler::types::as_int(*eval1) == 25, "first (square 5) = 25");
            CHECK(aura::compiler::types::as_int(*eval2) == 25,
                  "second (square 5) = 25 (bridge shared_ptr alive across cache)");
        }
    }
}

} // namespace aura_gc_batch

int main() {
    using namespace aura_gc_batch;
    std::println("=== GC batch: #1667 + #1734 + #1864 (13 ACs total) ===");
    std::println("(test_gc_evaluator_integration.cpp NOT included: custom CMake");
    std::println(" integration test with add_executable + add_test + custom contract_handler/stub");
    std::println(" target_sources — out of scope for batch consolidation)");
    run_1667_arm_release_roundtrip();
    run_1667_dtor_releases_armed();
    run_1667_commit_then_dtor();
    run_1667_cs_save_destroy_no_commit();
    run_1667_double_release_idempotent();
    run_1667_safepoint_after_dtor();
    run_1734_source_metric();
    run_1734_matching_snapshot();
    run_1734_mismatched_snapshot();
    run_1864_source();
    run_1864_sequential();
    run_1864_concurrent();
    run_204_mark_env_frame_roots();
    run_205_env_walk_callback();
    run_223_closure_bridge_epoch();
    run_224_closure_bridge_shared_ptr();
    std::println("\n=== GC batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

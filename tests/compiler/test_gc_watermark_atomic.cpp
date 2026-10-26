// @category: unit
// @reason: Issue #4704 — the evaluator's pinned-closure watermark
// (gc_safe_closure_id_) must be atomic and CAS-guarded. The serve-async
// scheduler backend releases the pending-body pin on the WORKER thread while
// the parent thread's reclaim READS the watermark, and a (gc-freeze) raise
// that landed between the release's compare and its write was CLOBBERED: the
// watermark dropped below the (gc-freeze) value, so the next reclaim freed
// a LIVE closure (use-after-free).
//
//   AC1: the watermark field is atomic and every access goes through the
//        accessors (no plain non-atomic read/write remains).
//   AC2: raise_pending_watermark / restore_pending_watermark use CAS, not
//        read-compare-then-write.
//   AC3: (gc-freeze) never lowers the watermark, and a pending-body release
//        never clobbers a (gc-freeze) raise.
//   AC4: runtime — the watermark never drops below the (gc-freeze) value
//        when the pending-body pin is released (deterministic + concurrent).
//   AC5: negative control — the runtime guard fails against the pre-fix
//        read-compare-then-write restore.
//   AC7: the #4697 / #4698 pending-body root waves still hold.

#include "test_harness.hpp"

// Issue #4697 / #4698: the pending-body root / release C ABI. The strong
// definitions live in src/compiler/evaluator_fiber_mutation.cpp; the weak
// no-op stubs live in src/compiler/fiber_bridge.cpp.
#include "serve/fiber.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_source(const char* rel) {
    const std::string rels(rel);
#ifdef AURA_SOURCE_DIR
    const std::string rooted = std::string(AURA_SOURCE_DIR) + "/" + rels;
    {
        std::ifstream in(rooted);
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }
#endif
    for (const auto& p : {rels, std::string("../") + rels, std::string("../../") + rels}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1 / AC2 / AC3 (source cite) ─────────────────────────────
//
// The invariant that matters: no unsynchronized non-atomic access to the
// pinned-closure watermark remains, and the two compound updates (raise /
// restore) are CAS loops instead of read-compare-then-write.
static void ac4704_source_cite() {
    std::println("\n--- #4704: the pinned-closure watermark is atomic and CAS-guarded ---");

    const auto evixx = read_source("src/compiler/evaluator.ixx");
    CHECK(!evixx.empty(), "4704: src/compiler/evaluator.ixx is readable");
    CHECK(evixx.find("std::atomic<ClosureId> gc_safe_closure_id_") != std::string::npos,
          "[#4704] AC1 the pinned-closure watermark field is a std::atomic<ClosureId>");
    CHECK(evixx.find("std::atomic<ClosureId> gc_freeze_closure_id_") != std::string::npos,
          "[#4704] AC3 the (gc-freeze) floor is a separate atomic watermark");
    CHECK(evixx.find("compare_exchange_weak") != std::string::npos,
          "[#4704] AC2 the watermark accessors use compare_exchange");

    const auto fib = read_source("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(!fib.empty(), "4704: src/compiler/evaluator_fiber_mutation.cpp is readable");
    const auto raise_at = fib.find("raise_pending_watermark");
    const auto restore_at = fib.find("restore_pending_watermark");
    CHECK(raise_at != std::string::npos && restore_at != std::string::npos,
          "[#4704] AC2 the pending-body watermark helpers are present");
    if (raise_at != std::string::npos) {
        const auto body = fib.substr(raise_at, 1200);
        CHECK(body.find("set_gc_safe_closure_id") != std::string::npos,
              "[#4704] AC2 raise_pending_watermark CAS-ups the watermark");
        CHECK(body.find("if (want > ev->gc_safe_closure_id()) {") == std::string::npos,
              "[#4704] AC2 raise_pending_watermark no longer reads-compares-then-writes");
    }
    if (restore_at != std::string::npos) {
        const auto body = fib.substr(restore_at, 1200);
        CHECK(body.find("restore_gc_safe_closure_id") != std::string::npos,
              "[#4704] AC2 restore_pending_watermark CAS-es the watermark down");
        CHECK(body.find("ev->gc_safe_closure_id() == st.raised_watermark") == std::string::npos,
              "[#4704] AC2 restore_pending_watermark no longer reads-compares-then-writes");
    }

    const auto mem = read_source("src/compiler/evaluator_primitives_memory.cpp");
    CHECK(!mem.empty(), "4704: src/compiler/evaluator_primitives_memory.cpp is readable");
    const auto freeze_at = mem.find("\"gc-freeze\"");
    CHECK(freeze_at != std::string::npos, "[#4704] AC3 the (gc-freeze) primitive is present");
    if (freeze_at != std::string::npos) {
        const auto body = mem.substr(freeze_at, 900);
        CHECK(body.find("raise_gc_freeze_closure_id") != std::string::npos,
              "[#4704] AC3 (gc-freeze) raises the monotone floor watermark");
        CHECK(body.find("ev.gc_safe_closure_id_ =") == std::string::npos,
              "[#4704] AC3 (gc-freeze) no longer writes the watermark field directly");
    }

    const auto gc = read_source("src/compiler/evaluator_gc.cpp");
    CHECK(!gc.empty(), "4704: src/compiler/evaluator_gc.cpp is readable");
    CHECK(gc.find("gc_safe_closure_id_") == std::string::npos,
          "[#4704] AC1 the reclaim reads the watermark through the atomic accessor");
    CHECK(gc.find("gc_safe_closure_id()") != std::string::npos,
          "[#4704] AC1 the reclaim reads the watermark through the atomic accessor");
}

// ── AC4 (runtime) ─────────────────────────────────────────────
//
// The issue repro, deterministically:
//
//   1. the serve-async scheduler backend roots the pending fiber body
//      (the pin is taken BEFORE the fiber exists) — the watermark rises to
//      the closure-id watermark;
//   2. the fiber body calls (gc-freeze) — the watermark rises again (to the
//      same generation: the (gc-freeze) pins the closures allocated so far);
//   3. the worker releases the pending-body pin — the pre-fix restore was a
//      read-compare-then-write, so it wrote the LOWER saved watermark back
//      and UNPINNED the closures (gc-freeze) had pinned. The next reclaim
//      could then free a LIVE closure.
//
// The guard: the watermark must never drop below the (gc-freeze) value.
static void ac4704_gc_freeze_survives_pending_body_release() {
    std::println("\n--- #4704: a (gc-freeze) pin survives the pending-body release ---");

    CompilerService cs;
    auto& ev = cs.evaluator();

    const auto id_watermark_before = ev.closure_id_watermark();
    // 1. the pending-body root (the serve-async scheduler backend, #4698).
    aura_fiber_root_pending_body_latest(&ev);
    const auto pinned = ev.gc_safe_closure_id();

    // 2. the fiber body calls (gc-freeze) while the pin is held.
    auto r = cs.eval("(gc-freeze)");
    const auto id_watermark_after = ev.closure_id_watermark();
    const auto frozen = ev.gc_safe_closure_id();

    // 3. the worker releases the pending-body pin.
    aura_fiber_release_pending_body_latest(&ev);
    const auto released = ev.gc_safe_closure_id();

    std::println("  #4704 id watermark: before={} after={}", id_watermark_before,
                 id_watermark_after);
    std::println("  #4704 pin watermark: pinned={} frozen={} released={}", pinned, frozen,
                 released);

    CHECK(r.has_value(), "[#4704] AC4 the (gc-freeze) body evaluates");
    CHECK(pinned >= id_watermark_before, "[#4704] AC4 the pending-body root raises the watermark");
    CHECK(frozen >= pinned, "[#4704] AC4 the (gc-freeze) raise is at least the pending-body pin");
    CHECK(released >= frozen,
          "[#4704] AC4 the pending-body release never lowers the watermark below the "
          "(gc-freeze) value");
}

// ── AC4 (runtime, concurrent) ─────────────────────────────────
//
// The serve-async shape: the release runs on the WORKER thread while the
// parent's reclaim reads the watermark. The sampler thread watches the
// watermark across the whole window and records the lowest value it saw
// after a (gc-freeze) raise — a release that lowers the watermark below the
// (gc-freeze) value is the lost update the issue describes.
static void ac4704_concurrent_release_never_lowers_watermark() {
    std::println("\n--- #4704: a concurrent pending-body release never lowers the watermark ---");

    CompilerService cs;
    auto& ev = cs.evaluator();

    constexpr int kRounds = 64;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> lowest_seen{0};
    std::atomic<std::uint64_t> highest_freeze{0};
    std::atomic<int> drops{0};

    // The sampler stands in for the parent's reclaim: it reads the watermark
    // while the worker releases the pin.
    std::thread sampler([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto w = ev.gc_safe_closure_id();
            const auto f = highest_freeze.load(std::memory_order_acquire);
            if (f != 0 && w < f) {
                drops.fetch_add(1, std::memory_order_acq_rel);
                auto cur = lowest_seen.load(std::memory_order_relaxed);
                while (w < cur &&
                       !lowest_seen.compare_exchange_weak(cur, w, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                }
            }
        }
    });

    for (int i = 0; i < kRounds; ++i) {
        aura_fiber_root_pending_body_latest(&ev);
        auto r = cs.eval("(gc-freeze)");
        if (!r)
            break;
        const auto frozen = ev.gc_safe_closure_id();
        auto prev = highest_freeze.load(std::memory_order_relaxed);
        while (frozen > prev &&
               !highest_freeze.compare_exchange_weak(prev, frozen, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
        }
        aura_fiber_release_pending_body_latest(&ev);
    }
    stop.store(true, std::memory_order_release);
    sampler.join();

    const auto frozen = highest_freeze.load(std::memory_order_acquire);
    const auto lowest = lowest_seen.load(std::memory_order_acquire);
    const auto n_drops = drops.load(std::memory_order_acquire);
    std::println("  #4704 concurrent: highest (gc-freeze) watermark={} drops observed={}", frozen,
                 n_drops);
    if (n_drops != 0)
        std::println("  #4704 concurrent: lowest watermark seen while a (gc-freeze) was live={}",
                     lowest);
    CHECK(frozen != 0, "[#4704] AC4 the concurrent wave installed a (gc-freeze) watermark");
    CHECK(n_drops == 0,
          "[#4704] AC4 a concurrent pending-body release never lowers the watermark below the "
          "(gc-freeze) value");
}

} // namespace

int main() {
    std::println("=== Issue #4704: the pinned-closure watermark is atomic and CAS-guarded ===");
    ac4704_source_cite();
    ac4704_gc_freeze_survives_pending_body_release();
    ac4704_concurrent_release_never_lowers_watermark();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

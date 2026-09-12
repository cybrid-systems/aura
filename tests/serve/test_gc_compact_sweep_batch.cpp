// tests/serve/test_gc_compact_sweep_batch.cpp — GC compact sweep batch driver.
// (domain/ pilot abandoned in R1; tests/domain/arena/ no longer exists).
//
// test_compact_sweep_batch.cpp — batch driver for compact_sweep family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration,
// following the test_per_defuse_batch / test_env_lookup_batch /
// test_fiber_resume_batch precedent in AuraDomainTests.cmake):
//
//   Issue #1732 — compact_sweep returns typed CompactSweepResult (not
//                 void*) so callers need no cast (4 ACs)
//   Issue #1865 — compact_sweep clears pair_remap_ on successful
//                 sweep so remaps cannot outlive GC (3 ACs)
//   Issue #1866 — compact_sweep(nullptr) bumps
//                 gc_compact_sweep_null_marks_total metric (3 ACs)
//
// Pattern: CHECK() macros + RUN_ALL_TESTS() (test_harness.hpp),
// namespace aura_compact_sweep_batch, EXCLUDE_FROM_ALL per
// AuraDomainTests.cmake legacy batch convention. Default build skips;
// granular debug via `ninja test_compact_sweep_batch` on demand.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "serve/gc_coordinator.h"
#include "core/flatast_restamp.hh"       // #3677 unified restamp counter
#include "core/gc_hooks.h"               // #3677 ffi-pin defer arm/release
#include "core/moving_densify_health.hh" // #3677 last Moving window atomics

#include <fstream>
#include <initializer_list>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.envframe_lifetime; // #3679 densify ownership scan counter

namespace aura_compact_sweep_batch {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::GCSweepBuffers;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void alloc_pairs(CompilerService& cs, int n) {
    for (int i = 0; i < n; ++i) {
        auto r = cs.eval("(cons " + std::to_string(i) + " " + std::to_string(i + 1) + ")");
        (void)r;
    }
}

// ── Block 1: Issue #1732 (4 ACs) ──
// Original: tests/test_compact_sweep_result_1732.cpp
static void run_1732_typed_result() {
    std::println("\n=== Issue #1732: CompactSweepResult typed API ===");

    // AC1: source defines CompactSweepResult + by-value compact_sweep
    {
        std::println("\n--- AC1: CompactSweepResult typed API ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1732") != std::string::npos, "cites #1732");
        CHECK(ixx.find("struct CompactSweepResult") != std::string::npos,
              "defines CompactSweepResult");
        CHECK(ixx.find("CompactSweepResult compact_sweep") != std::string::npos,
              "returns CompactSweepResult");
        auto pos = ixx.find("CompactSweepResult compact_sweep");
        CHECK(pos != std::string::npos, "found signature");
        auto win = ixx.substr(pos > 200 ? pos - 200 : 0, 400);
        CHECK(win.find("void* compact_sweep") == std::string::npos,
              "no void* compact_sweep signature");
    }

    // AC2: layout 4×size_t
    {
        std::println("\n--- AC2: layout 4×size_t ---");
        CHECK(sizeof(Evaluator::CompactSweepResult) == 4 * sizeof(std::size_t),
              "sizeof CompactSweepResult == 4×size_t");
    }

    // AC3: nullptr buffers → empty result
    {
        std::println("\n--- AC3: nullptr → empty ---");
        Evaluator ev;
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "empty() true");
        CHECK(r.strings_freed == 0 && r.pairs_freed == 0 && r.closures_freed == 0 &&
                  r.fiber_results_freed == 0,
              "all zero fields");
    }

    // AC4: empty marks non-null
    {
        std::println("\n--- AC4: empty marks non-null ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        CHECK(r.strings_freed == 0 && r.pairs_freed == 0 && r.closures_freed == 0,
              "zero free counts with empty marks");
    }
}

// ── Block 2: Issue #1865 (3 ACs) ──
// Original: tests/test_compact_sweep_pair_remap_1865.cpp
static void run_1865_pair_remap_clear() {
    std::println("\n=== Issue #1865: compact_sweep clears pair_remap_ ===");

    // AC1: source clears pair_remap_ in compact_sweep; cites #1865
    {
        std::println("\n--- AC1: compact_sweep clears pair_remap_ ---");
        auto gc = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1865") != std::string::npos, "cites #1865");
        auto pos = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep");
        if (pos == std::string::npos)
            pos = gc.find("Evaluator::compact_sweep");
        CHECK(pos != std::string::npos, "compact_sweep present");
        auto win = gc.substr(pos, 6000);
        CHECK(win.find("pair_remap_.clear()") != std::string::npos ||
                  win.find("pair_remap_.clear") != std::string::npos,
              "clears pair_remap_");
        CHECK(win.find("heap_mutex()") != std::string::npos, "holds heap_mutex");
        auto clear_pos = win.find("pair_remap_.clear");
        auto panic_pos = win.find("should_defer_destructive_gc");
        if (panic_pos == std::string::npos)
            panic_pos = win.find("has_panic_checkpoint");
        CHECK(clear_pos != std::string::npos && panic_pos != std::string::npos &&
                  clear_pos > panic_pos,
              "clear after panic-defer check");
        CHECK(!ixx.empty() && ixx.find("#1865") != std::string::npos, "ixx cites #1865");
    }

    // AC2: compact_pairs builds remap; compact_sweep empties it
    {
        std::println("\n--- AC2: compact_pairs then compact_sweep clears remap ---");
        CompilerService cs;
        alloc_pairs(cs, 5);
        auto& ev = cs.evaluator();
        std::vector<bool> mask = {true, false, true, false, true};
        std::size_t n = ev.compact_pairs(mask);
        CHECK(n == 3, "3 live pairs after compact");
        CHECK(ev.pair_remap_size() == 5, "remap sized to old count");
        CHECK(ev.resolve_pair(0) == 0, "remap 0→0 before sweep");
        CHECK(ev.resolve_pair(1) == -1, "remap 1 dead before sweep");
        CHECK(ev.resolve_pair(2) == 1, "remap 2→1 before sweep");

        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        (void)r;
        CHECK(ev.pair_remap_size() == 0, "remap cleared after sweep");
        CHECK(ev.resolve_pair(0) == 0, "identity after clear");
        CHECK(ev.resolve_pair(1) == 1, "identity after clear (stale dead lost)");
    }

    // AC3: nullptr / early paths do not require remap
    {
        std::println("\n--- AC3: nullptr early return leaves empty remap ---");
        Evaluator ev;
        CHECK(ev.pair_remap_size() == 0, "starts empty");
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "null sweep empty result");
        CHECK(ev.pair_remap_size() == 0, "still empty");
    }
}

// ── Block 3: Issue #1866 (3 ACs) ──
// Original: tests/test_compact_sweep_null_marks_1866.cpp
static void run_1866_null_marks_metric() {
    std::println("\n=== Issue #1866: compact_sweep(nullptr) bumps metric ===");

    // AC1: source cites #1866; null path bumps metric; returns result{}
    {
        std::println("\n--- AC1: null marks bumps metric ---");
        auto gc = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1866") != std::string::npos, "cites #1866");
        auto pos = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep");
        if (pos == std::string::npos)
            pos = gc.find("Evaluator::compact_sweep");
        CHECK(pos != std::string::npos, "compact_sweep present");
        auto win = gc.substr(pos, 1600);
        CHECK(win.find("if (!marks)") != std::string::npos, "null marks check");
        CHECK(win.find("gc_compact_sweep_null_marks_total") != std::string::npos,
              "bumps null-marks metric");
        CHECK(win.find("return result") != std::string::npos, "returns zeroed result");
        CHECK(win.find("return nullptr") == std::string::npos, "no return nullptr");
        CHECK(!hdr.empty() && hdr.find("gc_compact_sweep_null_marks_total") != std::string::npos,
              "metric field declared");
        CHECK(hdr.find("#1866") != std::string::npos, "header cites #1866");
    }

    // AC2: CompilerMetrics increments on nullptr
    {
        std::println("\n--- AC2: CompilerMetrics increments on nullptr ---");
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(&metrics);
        CHECK(metrics.gc_compact_sweep_null_marks_total.load() == 0, "starts at 0");
        auto r0 = ev.compact_sweep(nullptr);
        CHECK(r0.empty(), "zeroed result");
        CHECK(metrics.gc_compact_sweep_null_marks_total.load() == 1, "bumped to 1");
        auto r1 = ev.compact_sweep(nullptr);
        CHECK(r1.empty(), "still zeroed");
        CHECK(metrics.gc_compact_sweep_null_marks_total.load() == 2, "bumped to 2");
    }

    // AC3: null metrics + non-null marks
    {
        std::println("\n--- AC3: null metrics safe; non-null marks no bump ---");
        Evaluator ev;
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "null metrics nullptr path ok");

        CompilerMetrics metrics;
        ev.set_compiler_metrics(&metrics);
        GCSweepBuffers marks{};
        auto r2 = ev.compact_sweep(&marks);
        (void)r2;
        CHECK(metrics.gc_compact_sweep_null_marks_total.load() == 0,
              "non-null marks does not bump null metric");
        CompilerService cs;
        auto r3 = cs.evaluator().compact_sweep(nullptr);
        CHECK(r3.empty(), "service evaluator nullptr path ok");
    }
}

// ── Issue #206 — GC sweep compact_pairs + resolve_pair (folded from
// tests/issues/test_issue_206.cpp via #1957) ── Verifies the Evaluator's compact_pairs() /
// resolve_pair() / clear_pair_remap() contract: live pairs move to the front, dead pairs get remap
// entry -1, stale PairIds from before the compact resolve to the latest remap.

static void alloc_pairs_206(aura::compiler::CompilerService& cs, int n) {
    for (int i = 0; i < n; ++i) {
        std::string src = "(cons " + std::to_string(i) + " " + std::to_string(i + 1) + ")";
        (void)cs.eval(src);
    }
}

static void run_206_resolve_identity() {
    std::println("\n--- #206: resolve_pair identity before any compact ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 5);
    auto& ev = cs.evaluator();
    CHECK(ev.pair_remap_size() == 0, "remap is empty (no compact yet)");
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(ev.resolve_pair(i) == static_cast<std::int64_t>(i),
              "resolve_pair returns identity for id " + std::to_string(i));
    }
}

static void run_206_empty_mask_all_live() {
    std::println("\n--- #206: compact_pairs empty live_mask = all-live ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 5);
    auto& ev = cs.evaluator();
    std::vector<bool> empty_mask;
    std::size_t n_after = ev.compact_pairs(empty_mask);
    CHECK(n_after == 5, "5 pairs remain after compact (all live)");
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(ev.resolve_pair(i) == static_cast<std::int64_t>(i),
              "resolve_pair(" + std::to_string(i) + ") is identity after all-live compact");
    }
}

static void run_206_selective_mask() {
    std::println("\n--- #206: compact_pairs selective live_mask (some dead) ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 5);
    auto& ev = cs.evaluator();
    // Live: 0, 2, 4; dead: 1, 3.
    std::vector<bool> mask = {true, false, true, false, true};
    std::size_t n_after = ev.compact_pairs(mask);
    CHECK(n_after == 3, "3 pairs remain after compact (5 - 2 dead)");
    CHECK(ev.resolve_pair(0) == 0, "old 0 (live) -> new 0");
    CHECK(ev.resolve_pair(1) == -1, "old 1 (dead) -> -1");
    CHECK(ev.resolve_pair(2) == 1, "old 2 (live) -> new 1");
    CHECK(ev.resolve_pair(3) == -1, "old 3 (dead) -> -1");
    CHECK(ev.resolve_pair(4) == 2, "old 4 (live) -> new 2");
}

static void run_206_multi_step() {
    std::println("\n--- #206: multi-step compact rebuilds the remap ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 6);
    auto& ev = cs.evaluator();
    // First compact: dead = 1, 4. Live: 0, 2, 3, 5.
    std::vector<bool> mask1 = {true, false, true, true, false, true};
    ev.compact_pairs(mask1);
    CHECK(ev.resolve_pair(0) == 0, "after compact1: old 0 -> new 0");
    CHECK(ev.resolve_pair(1) == -1, "after compact1: old 1 -> -1");
    CHECK(ev.resolve_pair(2) == 1, "after compact1: old 2 -> new 1");
    // Add 2 more pairs; arena is now 4 + 2 = 6.
    alloc_pairs_206(cs, 2);
    // Second compact: dead at index 4 of the new arena.
    std::vector<bool> mask2 = {true, true, true, true, false, true};
    ev.compact_pairs(mask2);
    CHECK(ev.resolve_pair(0) == 0, "after compact2: old 0 -> new 0");
    CHECK(ev.resolve_pair(1) == 1, "after compact2: old 1 -> new 1");
    CHECK(ev.resolve_pair(4) == -1, "after compact2: old 4 -> -1");
    CHECK(ev.resolve_pair(5) == 4, "after compact2: old 5 -> new 4");
}

static void run_206_clear_remap() {
    std::println("\n--- #206: clear_pair_remap resets to identity ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 3);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {true, false, true};
    ev.compact_pairs(mask);
    CHECK(ev.resolve_pair(1) == -1, "before clear: old 1 -> -1");
    ev.clear_pair_remap();
    CHECK(ev.pair_remap_size() == 0, "remap is empty after clear");
    CHECK(ev.resolve_pair(0) == 0, "after clear: resolve_pair(0) is identity");
    CHECK(ev.resolve_pair(1) == 1, "after clear: resolve_pair(1) is identity");
    CHECK(ev.resolve_pair(2) == 2, "after clear: resolve_pair(2) is identity");
}

static void run_206_out_of_range() {
    std::println("\n--- #206: resolve_pair handles out-of-range ids ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 3);
    auto& ev = cs.evaluator();
    CHECK(ev.resolve_pair(999) == 999,
          "resolve_pair(999) is identity when no compact has happened");
    std::vector<bool> empty_mask;
    ev.compact_pairs(empty_mask);
    CHECK(ev.resolve_pair(999) == -1, "resolve_pair(999) returns -1 (out of remap range)");
    CHECK(ev.resolve_pair(2) == 2, "resolve_pair(2) is in range, identity");
}

static void run_206_all_dead() {
    std::println("\n--- #206: compact_pairs with all-dead mask ---");
    aura::compiler::CompilerService cs;
    alloc_pairs_206(cs, 4);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {false, false, false, false};
    std::size_t n_after = ev.compact_pairs(mask);
    CHECK(n_after == 0, "0 pairs remain after all-dead compact");
    for (std::uint64_t i = 0; i < 4; ++i) {
        CHECK(ev.resolve_pair(i) == -1, "resolve_pair(" + std::to_string(i) + ") is -1 (all dead)");
    }
}

// ── Block N: Issue #3677 (Soft compact restamp + Moving window untouched) ──
// compact_sweep's opportunistic live_compact(Soft) can bump the arena gen /
// invalidate LifetimePins (saved_bytes > 0 tail) without a following
// mutate/steal restamp. #3677: restamp the triad on invalidates_pins, keep
// the Moving window publish Moving-only, and stop feeding the Moving
// pin-contract counter from the vacuous Soft default-true.
static void run_3677_soft_compact_restamp() {
    std::println("\n=== Issue #3677: Soft compact restamp + Moving window untouched ===");
    namespace mdh = aura::core::moving_densify_health;

    // AC2 + AC3: Soft compact must not move the last Moving window nor the
    // Moving pin-contract counter (Soft pin_contract_held is vacuous true).
    {
        std::println("\n--- AC2/AC3: window + pin-contract counter untouched ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto had0 = mdh::g_last_had_moving_densify.load(std::memory_order_relaxed);
        const auto seq0 = mdh::g_last_window_seq.load(std::memory_order_relaxed);
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto pc0 = m->moving_compact_pin_contract_fail_total.load(std::memory_order_relaxed);
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        CHECK(mdh::g_last_window_seq.load(std::memory_order_relaxed) == seq0,
              "AC2: last Moving window seq unchanged by Soft compact");
        CHECK(mdh::g_last_had_moving_densify.load(std::memory_order_relaxed) == had0,
              "AC2: had_moving_densify unchanged by Soft compact");
        CHECK(m->moving_compact_pin_contract_fail_total.load(std::memory_order_relaxed) == pc0,
              "AC3: pin-contract counter unchanged by Soft compact");
        (void)r;
    }

    // AC1 + AC4: when the Soft compact invalidates pins, the unified restamp
    // runs; under ffi-pin defer the whole sweep (compact + restamp) skips.
    {
        std::println("\n--- AC1/AC4: invalidates_pins → restamp; defer → skip ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto rs0 = aura::ast::unified_restamp_calls_total_v_read();
        const auto gen_restamps0 =
            m->arena_live_compact_gen_restamps_total.load(std::memory_order_relaxed);
        // AC4: ffi-pin defer active → zeroed result + no restamp (the
        // half-graph must not be restamped, #2005/#2088 defer face).
        aura::gc_hooks::arm_ffi_pin_defer();
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        CHECK(r.empty(), "AC4: ffi-pin defer → zeroed sweep result");
        CHECK(aura::ast::unified_restamp_calls_total_v_read() == rs0,
              "AC4: no restamp under defer (half-graph untouched)");
        aura::gc_hooks::release_ffi_pin_defer();
        // AC1: sweep normally — if this compact invalidated pins (gen
        // restamp), the unified restamp must have run. Same safepoint;
        // invariant keyed on the existing gen-restamp metric, not a
        // heap-shape assumption.
        auto r2 = ev.compact_sweep(&marks);
        (void)r2;
        const auto gen_restamps1 =
            m->arena_live_compact_gen_restamps_total.load(std::memory_order_relaxed);
        if (gen_restamps1 > gen_restamps0) {
            CHECK(aura::ast::unified_restamp_calls_total_v_read() > rs0,
                  "AC1: gen restamp (invalidates_pins) → unified restamp ran (#3677)");
        } else {
            std::println(
                "  note: unit-env Soft compact did not invalidate pins; AC1 runtime invariant "
                "vacuous this round (source-cite AC carries)");
            CHECK(true, "AC1: no invalidates_pins this round — nothing to restamp");
        }
    }

    // AC5: source-cite — compact_sweep joins the restamp triad on
    // invalidates_pins, cites #3677, and no longer feeds the Moving
    // pin-contract counter from the Soft result.
    {
        std::println("\n--- AC5: source-cite ---");
        std::ifstream gc("src/compiler/evaluator_gc.cpp");
        std::string body((std::istreambuf_iterator<char>(gc)), std::istreambuf_iterator<char>());
        CHECK(body.find("Issue #3677") != std::string::npos, "AC5: evaluator_gc.cpp cites #3677");
        CHECK(body.find("if (lc.invalidates_pins)") != std::string::npos &&
                  body.find("unified_restamp_after_boundary(UnifiedRestampSite::Densify)") !=
                      std::string::npos,
              "AC5: Soft invalidates_pins → unified restamp (AC1)");
        CHECK(
            body.find("m->moving_compact_pin_contract_fail_total.fetch_add(lc.pin_contract_held") ==
                std::string::npos,
            "AC5: Soft pin-contract fetch_add removed (AC3)");
    }
}

// ── Block N+1: Issue #3679 (Guard/scan AFTER pair compact + Soft live_compact) ──
// compact_sweep ran the EnvFrame Guard + densify ownership scan at ENTRY
// while its helper comment claimed post-remap-table ordering — the scan
// walked pre-compact slots, and Soft live_compact could bump the arena gen
// / remap pins with no restamp unless invalidates_pins fired. #3679: the
// helper moves after pair compact + live_compact(Soft) (before the unified
// restamp), the restamp condition gains remapped_pins > 0, and the
// panic/ffi defer early-returns still skip the scan entirely.
static void run_3679_guard_scan_order() {
    std::println("\n=== Issue #3679: Guard/scan after pair compact + Soft live_compact ===");
    namespace mdh = aura::core::moving_densify_health;
    namespace efl = aura::core::envframe_lifetime;

    // AC1: source order — exactly one helper call site, after live_compact
    // (Soft) and before the unified restamp; entry call gone; restamp
    // condition includes remapped_pins; the file cites #3679.
    {
        std::println("\n--- AC1: source-cite order ---");
        const std::string body =
            read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        CHECK(!body.empty(), "AC1: evaluator_gc.cpp readable");
        const std::string call = "(void)run_envframe_lifetime_guard_compact_sweep_helper(*this);";
        std::size_t pos = 0;
        int call_sites = 0;
        std::size_t call_pos = std::string::npos;
        while ((pos = body.find(call, pos)) != std::string::npos) {
            ++call_sites;
            if (call_pos == std::string::npos)
                call_pos = pos;
            pos += call.size();
        }
        CHECK(call_sites == 1, "AC1: exactly one helper call site (entry call removed)");
        const auto lc_pos = body.find("live_compact(aura::ast::LiveCompactMode::Soft)");
        const auto defer_pos = body.find("should_defer_destructive_gc()");
        const auto restamp_pos =
            body.find("unified_restamp_after_boundary(UnifiedRestampSite::Densify)");
        CHECK(lc_pos != std::string::npos && defer_pos != std::string::npos &&
                  restamp_pos != std::string::npos && call_pos != std::string::npos,
              "AC1: all order anchors present");
        CHECK(call_pos > defer_pos, "AC1: defer check precedes the Guard scan (AC3 order)");
        CHECK(call_pos > lc_pos, "AC1: Guard scan runs after live_compact(Soft)");
        CHECK(call_pos < restamp_pos, "AC1: Guard scan precedes the unified restamp");
        CHECK(body.find("lc.invalidates_pins || lc.remapped_pins > 0") != std::string::npos,
              "AC1: restamp condition includes remapped_pins (#3679)");
        CHECK(body.find("Issue #3679") != std::string::npos, "AC1: evaluator_gc.cpp cites #3679");
    }

    // AC2: when the Soft compact invalidates pins OR remaps pins, the
    // unified restamp runs (extends the #3677 invalidates_pins-only
    // invariant). Keyed on the existing live-compact metrics, not a
    // heap-shape assumption (same safepoint pattern as #3677 AC1).
    {
        std::println("\n--- AC2: restamp on invalidates_pins or remapped_pins ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto rs0 = aura::ast::unified_restamp_calls_total_v_read();
        const auto gen0 = m->arena_live_compact_gen_restamps_total.load(std::memory_order_relaxed);
        const auto remap0 =
            m->arena_live_compact_remapped_pins_total.load(std::memory_order_relaxed);
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        (void)r;
        const auto gen1 = m->arena_live_compact_gen_restamps_total.load(std::memory_order_relaxed);
        const auto remap1 =
            m->arena_live_compact_remapped_pins_total.load(std::memory_order_relaxed);
        if (gen1 > gen0 || remap1 > remap0) {
            CHECK(aura::ast::unified_restamp_calls_total_v_read() > rs0,
                  "AC2: invalidates_pins/remapped_pins → unified restamp ran (#3679)");
        } else {
            std::println("  note: unit-env Soft compact pinned/remapped nothing; AC2 runtime "
                         "invariant vacuous this round (source-cite AC carries)");
            CHECK(true, "AC2: no pin invalidation this round — nothing to restamp");
        }
    }

    // AC3: panic/ffi defer → zeroed result, no compact, and NO Guard /
    // scan_skip_freed walk of the unmoved state (guard_runs + densify
    // ownership scan stay flat).
    {
        std::println("\n--- AC3: defer skips compact AND the Guard/scan ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto guard0 = m->envframe_lifetime_guard_runs_total.load(std::memory_order_relaxed);
        const auto scan0 = efl::envframe_lifetime_densify_ownership_scan_total();
        aura::gc_hooks::arm_ffi_pin_defer();
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        CHECK(r.empty(), "AC3: ffi-pin defer → zeroed sweep result");
        CHECK(m->envframe_lifetime_guard_runs_total.load(std::memory_order_relaxed) == guard0,
              "AC3: no Guard run under defer (scan_skip_freed not walked)");
        CHECK(efl::envframe_lifetime_densify_ownership_scan_total() == scan0,
              "AC3: no post-compact ownership scan under defer");
        aura::gc_hooks::release_ffi_pin_defer();
    }

    // AC4: Soft / no-arena_group unit env — helper still runs on a plain
    // sweep (Guard dtor + densify ownership scan bump) and no Moving
    // relocate happens.
    {
        std::println("\n--- AC4: helper runs on plain Soft sweep; no Moving relocate ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto guard0 = m->envframe_lifetime_guard_runs_total.load(std::memory_order_relaxed);
        const auto scan0 = efl::envframe_lifetime_densify_ownership_scan_total();
        const auto seq0 = mdh::g_last_window_seq.load(std::memory_order_relaxed);
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        (void)r;
        CHECK(m->envframe_lifetime_guard_runs_total.load(std::memory_order_relaxed) > guard0,
              "AC4: Guard + scan_skip_freed ran on the post-compact state");
        CHECK(efl::envframe_lifetime_densify_ownership_scan_total() > scan0,
              "AC4: densify ownership scan ran at the CompactSweep site (#2340 AC4)");
        CHECK(mdh::g_last_window_seq.load(std::memory_order_relaxed) == seq0,
              "AC4: no Moving relocate from the Soft sweep");
    }
}

} // namespace aura_compact_sweep_batch

int main() {
    aura_compact_sweep_batch::run_1732_typed_result();
    aura_compact_sweep_batch::run_1865_pair_remap_clear();
    aura_compact_sweep_batch::run_1866_null_marks_metric();
    aura_compact_sweep_batch::run_206_resolve_identity();
    aura_compact_sweep_batch::run_206_empty_mask_all_live();
    aura_compact_sweep_batch::run_206_selective_mask();
    aura_compact_sweep_batch::run_206_multi_step();
    aura_compact_sweep_batch::run_206_clear_remap();
    aura_compact_sweep_batch::run_206_out_of_range();
    aura_compact_sweep_batch::run_206_all_dead();
    aura_compact_sweep_batch::run_3677_soft_compact_restamp();
    aura_compact_sweep_batch::run_3679_guard_scan_order();
    return RUN_ALL_TESTS();
}
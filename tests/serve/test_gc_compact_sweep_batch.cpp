// tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
// (was tests/test_compact_sweep_batch.cpp). Prefer this path; do not re-add under tests/ root.
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

#include <fstream>
#include <initializer_list>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

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
        auto win = gc.substr(pos, 2200);
        CHECK(win.find("pair_remap_.clear()") != std::string::npos ||
                  win.find("pair_remap_.clear") != std::string::npos,
              "clears pair_remap_");
        CHECK(win.find("heap_mutex()") != std::string::npos, "holds heap_mutex");
        auto clear_pos = win.find("pair_remap_.clear");
        auto panic_pos = win.find("should_defer_compact_for_pending_checkpoint");
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
    return RUN_ALL_TESTS();
}
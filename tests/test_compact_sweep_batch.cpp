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

} // namespace aura_compact_sweep_batch

int main() {
    aura_compact_sweep_batch::run_1732_typed_result();
    aura_compact_sweep_batch::run_1865_pair_remap_clear();
    aura_compact_sweep_batch::run_1866_null_marks_metric();
    return RUN_ALL_TESTS();
}
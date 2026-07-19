// @category: unit
// @reason: Issue #1866 — compact_sweep(nullptr) must not be a silent
// failure: bump gc_compact_sweep_null_marks_total and return a zeroed
// CompactSweepResult (#1732 by-value; not nullptr).
//
//   AC1: source cites #1866; null path bumps metric; returns result{}
//   AC2: wired CompilerMetrics increments on nullptr
//   AC3: null metrics pointer does not crash; non-null marks no bump

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "serve/gc_coordinator.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::GCSweepBuffers;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: source ──
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
        // Include #1866 comment block + null-marks branch (body is long).
        auto win = gc.substr(pos, 1600);
        CHECK(win.find("if (!marks)") != std::string::npos, "null marks check");
        CHECK(win.find("gc_compact_sweep_null_marks_total") != std::string::npos,
              "bumps null-marks metric");
        CHECK(win.find("return result") != std::string::npos, "returns zeroed result");
        // Must not reintroduce void* nullptr return on this path.
        CHECK(win.find("return nullptr") == std::string::npos, "no return nullptr");
        CHECK(!hdr.empty() && hdr.find("gc_compact_sweep_null_marks_total") != std::string::npos,
              "metric field declared");
        CHECK(hdr.find("#1866") != std::string::npos, "header cites #1866");
    }

    // ── AC2: metric increments ──
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

    // ── AC3: null metrics + non-null marks ──
    {
        std::println("\n--- AC3: null metrics safe; non-null marks no bump ---");
        Evaluator ev;
        // No metrics wired — must not crash.
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "null metrics nullptr path ok");

        CompilerMetrics metrics;
        ev.set_compiler_metrics(&metrics);
        GCSweepBuffers marks{};
        auto r2 = ev.compact_sweep(&marks);
        (void)r2;
        CHECK(metrics.gc_compact_sweep_null_marks_total.load() == 0,
              "non-null marks does not bump null metric");
        // Service-wired path still works.
        CompilerService cs;
        auto r3 = cs.evaluator().compact_sweep(nullptr);
        CHECK(r3.empty(), "service evaluator nullptr path ok");
    }

    std::println("\n=== test_compact_sweep_null_marks_1866: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #1732 — compact_sweep returns typed CompactSweepResult
// (not void*) so callers need no cast.
//
//   AC1: source defines CompactSweepResult + by-value compact_sweep
//   AC2: sizeof CompactSweepResult == 4×size_t
//   AC3: nullptr buffers → zeroed / empty result
//   AC4: non-null empty marks → zeroed counts (no crash)

#include "test_harness.hpp"
#include "serve/gc_coordinator.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

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

} // namespace

int main() {
    // ── AC1: source ──
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
        // No longer returns void*
        auto pos = ixx.find("CompactSweepResult compact_sweep");
        CHECK(pos != std::string::npos, "found signature");
        auto win = ixx.substr(pos > 200 ? pos - 200 : 0, 400);
        CHECK(win.find("void* compact_sweep") == std::string::npos,
              "no void* compact_sweep signature");
    }

    // ── AC2: layout ──
    {
        std::println("\n--- AC2: layout 4×size_t ---");
        CHECK(sizeof(Evaluator::CompactSweepResult) == 4 * sizeof(std::size_t),
              "sizeof CompactSweepResult == 4×size_t");
    }

    // ── AC3: null buffers ──
    {
        std::println("\n--- AC3: nullptr → empty ---");
        Evaluator ev;
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "empty() true");
        CHECK(r.strings_freed == 0 && r.pairs_freed == 0 && r.closures_freed == 0 &&
                  r.fiber_results_freed == 0,
              "all zero fields");
    }

    // ── AC4: empty marks ──
    {
        std::println("\n--- AC4: empty marks non-null ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        GCSweepBuffers marks{};
        auto r = ev.compact_sweep(&marks);
        CHECK(r.strings_freed == 0 && r.pairs_freed == 0 && r.closures_freed == 0,
              "zero free counts with empty marks");
    }

    std::println("\n=== test_compact_sweep_result_1732: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

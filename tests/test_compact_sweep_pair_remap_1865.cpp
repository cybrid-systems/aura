// @category: unit
// @reason: Issue #1865 — compact_sweep must clear pair_remap_ on the
// successful sweep path so remaps from compact_pairs cannot outlive GC.
//
//   AC1: source clears pair_remap_ in compact_sweep; cites #1865
//   AC2: compact_pairs builds remap; compact_sweep empties it
//   AC3: nullptr / early paths do not require remap (null does not clear
//        needed state — empty evaluator; panic path not armed here)

#include "test_harness.hpp"
#include "serve/gc_coordinator.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

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

std::string read_first(std::initializer_list<const char*> paths) {
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

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: compact_sweep clears pair_remap_ ---");
        auto gc = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1865") != std::string::npos, "cites #1865");
        // Prefer definition (return type prefix); fall back to any match.
        auto pos = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep");
        if (pos == std::string::npos)
            pos = gc.find("Evaluator::compact_sweep");
        CHECK(pos != std::string::npos, "compact_sweep present");
        // Body is long (panic defer + marks); window must cover clear.
        auto win = gc.substr(pos, 2200);
        CHECK(win.find("pair_remap_.clear()") != std::string::npos ||
                  win.find("pair_remap_.clear") != std::string::npos,
              "clears pair_remap_");
        // Clear must be on the successful path (after heap_mutex), not only
        // behind a free-count branch that might skip.
        CHECK(win.find("heap_mutex()") != std::string::npos, "holds heap_mutex");
        // Clear is after panic early-return, not before marks null-check only.
        auto clear_pos = win.find("pair_remap_.clear");
        auto panic_pos = win.find("should_defer_compact_for_pending_checkpoint");
        CHECK(clear_pos != std::string::npos && panic_pos != std::string::npos &&
                  clear_pos > panic_pos,
              "clear after panic-defer check");
        CHECK(!ixx.empty() && ixx.find("#1865") != std::string::npos, "ixx cites #1865");
    }

    // ── AC2: remap then sweep ──
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
        // Empty remap → identity (documented resolve_pair contract).
        CHECK(ev.resolve_pair(0) == 0, "identity after clear");
        CHECK(ev.resolve_pair(1) == 1, "identity after clear (stale dead lost)");
    }

    // ── AC3: nullptr does not crash; no spurious need for remap ──
    {
        std::println("\n--- AC3: nullptr early return leaves empty remap ---");
        Evaluator ev;
        CHECK(ev.pair_remap_size() == 0, "starts empty");
        auto r = ev.compact_sweep(nullptr);
        CHECK(r.empty(), "null sweep empty result");
        CHECK(ev.pair_remap_size() == 0, "still empty");
    }

    std::println("\n=== test_compact_sweep_pair_remap_1865: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #1755 — validate_linear_ownership_state must reject
// bridge_epoch drift and optionally bump
// linear_validate_bridge_epoch_drift_total.
//
//   AC1: source cites #1755; bridge mismatch path present
//   AC2: metric field declared in observability_metrics.h
//   AC3: matching epochs → true; no drift counter bump
//   AC4: mismatched epochs (bridge != 0) → false + counter +1
//   AC5: bridge_epoch==0 skips bridge half (unbridged ok)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::Evaluator;
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
    // ── AC1/AC2: source + metric ──
    {
        std::println("\n--- AC1/AC2: #1755 source + metric field ---");
        std::string gc;
        for (const char* p :
             {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
            gc = read_file(p);
            if (!gc.empty())
                break;
        }
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1755") != std::string::npos, "cites #1755");
        CHECK(gc.find("linear_validate_bridge_epoch_drift_total") != std::string::npos ||
                  gc.find("bridge_epoch_drift_counter") != std::string::npos,
              "drift counter wiring");
        CHECK(gc.find("bridge_epoch != current_bridge_epoch") != std::string::npos ||
                  gc.find("bridge_epoch != 0 && bridge_epoch != current_bridge_epoch") !=
                      std::string::npos,
              "compares bridge epochs");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("linear_validate_bridge_epoch_drift_total") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: match → ok, no bump ──
    {
        std::println("\n--- AC3: matching epochs ---");
        std::atomic<std::uint64_t> drift{0};
        const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 5, 5, &drift);
        CHECK(ok, "Owned + matching bridge ok");
        CHECK(drift.load() == 0, "no drift bump on match");
    }

    // ── AC4: mismatch → fail + bump ──
    {
        std::println("\n--- AC4: mismatched bridge_epoch ---");
        std::atomic<std::uint64_t> drift{0};
        const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 4, 5, &drift);
        CHECK(!ok, "Owned + mismatched bridge fails");
        CHECK(drift.load() == 1, "drift counter +1");
        // Second call accumulates
        CHECK(!Evaluator::validate_linear_ownership_state(1, 10, 10, 3, 7, &drift),
              "second mismatch fails");
        CHECK(drift.load() == 2, "drift counter +2");
    }

    // ── AC5: unbridged (epoch 0) skips bridge half ──
    {
        std::println("\n--- AC5: bridge_epoch==0 skips bridge check ---");
        std::atomic<std::uint64_t> drift{0};
        const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 0, 99, &drift);
        CHECK(ok, "unbridged Owned ok");
        CHECK(drift.load() == 0, "no drift bump when bridge_epoch==0");
    }

    std::println("\n=== test_linear_validate_bridge_epoch_1755: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

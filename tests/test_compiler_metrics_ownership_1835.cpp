// @category: unit
// @reason: Issue #1835 — compiler_metrics_ ownership is a documented
// non-owning raw pointer (CompilerService::metrics_ lifetime), not a
// concurrent-rebind shared_ptr. Stats primitives null-check then use;
// concurrent set_compiler_metrics mid-eval is unsupported.
//
//   AC1: evaluator.ixx cites #1835 ownership contract
//   AC2: set_compiler_metrics wires pointer; null disables bumps
//   AC3: CompilerService still exposes metrics via stats primitive

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
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
    // ── AC1: source ──
    {
        std::println("\n--- AC1: ownership contract documented ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator.ixx");
        CHECK(src.find("#1835") != std::string::npos, "cites #1835");
        CHECK(src.find("non-owning raw pointer") != std::string::npos ||
                  src.find("non-owning") != std::string::npos,
              "documents non-owning");
        CHECK(src.find("set_compiler_metrics") != std::string::npos, "setter present");
        CHECK(src.find("CompilerService") != std::string::npos, "mentions CompilerService owner");
    }

    // ── AC2: null / rebind under single-thread quiescence ──
    {
        std::println("\n--- AC2: set/clear compiler_metrics under quiescence ---");
        Evaluator ev;
        CompilerMetrics metrics;
        CHECK(ev.compiler_metrics() == nullptr, "starts null");
        ev.set_compiler_metrics(&metrics);
        CHECK(ev.compiler_metrics() == &metrics, "wired");
        ev.set_compiler_metrics(nullptr);
        CHECK(ev.compiler_metrics() == nullptr, "cleared");
        // Rebind (still single-thread, no concurrent stats) is allowed.
        ev.set_compiler_metrics(&metrics);
        CHECK(ev.compiler_metrics() == &metrics, "rewired");
    }

    // ── AC3: service still has live metrics for stats ──
    {
        std::println("\n--- AC3: CompilerService metrics via engine:metrics ---");
        CompilerService cs;
        CHECK(cs.evaluator().compiler_metrics() != nullptr, "service wires metrics");
        auto r = cs.eval("(engine:metrics \"compile:type-cache-stats\")");
        CHECK(r.has_value(), "type-cache-stats runs with wired metrics");
    }

    std::println("\n=== test_compiler_metrics_ownership_1835: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

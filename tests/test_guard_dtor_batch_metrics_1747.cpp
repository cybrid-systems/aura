// @category: unit
// @reason: Issue #1747 — MutationBoundaryGuard dtor batches hold metrics
// locally (BatchMutationMetrics) then publishes ≤6 atomics on the common
// path instead of 15+ scattered fetch_add/CAS.
//
//   AC1: source cites #1747 + BatchMutationMetrics; common-path publish block
//   AC2: outermost Guard still bumps hold counters by 1
//   AC3: nested Guard does not double-count holds_total
//   AC4: short hold does not bump too_long / starvation counters

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

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

std::string dtor_window(const std::string& src) {
    auto pos = src.find("~MutationBoundaryGuard()");
    if (pos == std::string::npos)
        return {};
    // Capture hold-metrics region until post-boundary linear block.
    auto end = src.find("post-boundary linear closed-loop", pos);
    if (end == std::string::npos)
        end = pos + 5500;
    return src.substr(pos, end - pos);
}

// Count fetch_add / store / CAS in the common publish block (between
// "publish common path" and "Optional / rare path").
std::size_t count_common_path_atomics(const std::string& win) {
    auto a = win.find("publish common path");
    auto b = win.find("Optional / rare path");
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return 999;
    auto slice = win.substr(a, b - a);
    std::size_t n = 0;
    for (std::string_view needle : {"fetch_add", ".store(", "compare_exchange"}) {
        std::size_t p = 0;
        while ((p = slice.find(needle, p)) != std::string::npos) {
            ++n;
            p += needle.size();
        }
    }
    return n;
}

} // namespace

int main() {
    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: BatchMutationMetrics + ≤6 common-path atomics ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1747") != std::string::npos, "cites #1747");
        CHECK(ixx.find("BatchMutationMetrics") != std::string::npos,
              "BatchMutationMetrics present");
        auto win = dtor_window(ixx);
        CHECK(!win.empty(), "found dtor window");
        CHECK(win.find("BatchMutationMetrics") != std::string::npos, "batch in dtor");
        const auto n = count_common_path_atomics(win);
        CHECK(n <= 6, "common-path atomic writes ≤6");
        std::println("  (common-path atomic ops counted: {})", n);
    }

    // ── AC2: outermost bumps hold counters ──
    {
        std::println("\n--- AC2: outermost hold counters +1 ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        const auto s0 = m->mutation_hold_samples.load(std::memory_order_relaxed);
        const auto t0 = m->mutation_hold_duration_us_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            CHECK(ok, "guard acquired");
        }
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) == h0 + 1,
              "holds_total +1");
        CHECK(m->mutation_hold_samples.load(std::memory_order_relaxed) == s0 + 1, "samples +1");
        CHECK(m->mutation_hold_duration_us_total.load(std::memory_order_relaxed) >= t0,
              "duration total non-decreasing");
    }

    // ── AC3: nested does not double-count ──
    {
        std::println("\n--- AC3: nested Guard single holds sample ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            {
                Evaluator::MutationBoundaryGuard inner(ev, &ok);
            }
        }
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) == h0 + 1,
              "nested pair → holds_total +1 (outermost only)");
    }

    // ── AC4: short hold does not trip too_long ──
    {
        std::println("\n--- AC4: short hold no too_long ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        // Ensure threshold is high enough that a no-op guard is short.
        m->long_mutation_threshold_us.store(500'000, std::memory_order_relaxed);
        const auto tl0 = m->mutation_too_long_total.load(std::memory_order_relaxed);
        const auto st0 = m->starvation_prevented_count.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
        }
        CHECK(m->mutation_too_long_total.load(std::memory_order_relaxed) == tl0,
              "too_long unchanged");
        CHECK(m->starvation_prevented_count.load(std::memory_order_relaxed) == st0,
              "starvation_prevented unchanged");
    }

    std::println("\n=== test_guard_dtor_batch_metrics_1747: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #1764 — MutationBoundaryGuard enter_ts_ is
// std::optional; dtor must not use time_since_epoch().count() != 0
// as a sentinel for "outermost hold clock armed".
//
//   AC1: source cites #1764; optional enter_ts_ + has_value()
//   AC2: no time_since_epoch().count() != 0 sentinel on enter_ts_
//   AC3: outermost Guard still bumps hold counters
//   AC4: nested Guard does not double-count holds

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
    auto end = src.find("post-boundary linear closed-loop", pos);
    if (end == std::string::npos)
        end = pos + 5500;
    return src.substr(pos, end - pos);
}

} // namespace

int main() {
    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: optional enter_ts_ + no magic sentinel ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1764") != std::string::npos, "cites #1764");
        CHECK(ixx.find("std::optional<std::chrono::steady_clock::time_point> enter_ts_") !=
                  std::string::npos,
              "enter_ts_ is optional");
        CHECK(ixx.find("enter_ts_.has_value()") != std::string::npos, "dtor uses has_value()");

        auto win = dtor_window(ixx);
        CHECK(!win.empty(), "found dtor window");
        CHECK(win.find("enter_ts_.has_value()") != std::string::npos, "has_value in dtor");
        // The old magic sentinel must not remain on enter_ts_ in the dtor.
        CHECK(win.find("enter_ts_.time_since_epoch().count() != 0") == std::string::npos,
              "no time_since_epoch sentinel on enter_ts_");
        // Prefer optional assignment still present in ctor region.
        CHECK(ixx.find("enter_ts_ = std::chrono::steady_clock::now()") != std::string::npos,
              "ctor still arms enter_ts_ for outermost");
    }

    // ── AC3: outermost hold counters ──
    {
        std::println("\n--- AC3: outermost hold counters +1 ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        const auto s0 = m->mutation_hold_samples.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            CHECK(ok, "guard acquired");
            CHECK(g.is_outermost(), "single guard is outermost");
        }
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) == h0 + 1,
              "holds_total +1");
        CHECK(m->mutation_hold_samples.load(std::memory_order_relaxed) == s0 + 1, "samples +1");
    }

    // ── AC4: nested single sample ──
    {
        std::println("\n--- AC4: nested Guard single holds sample ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            {
                Evaluator::MutationBoundaryGuard inner(ev, &ok);
                CHECK(!inner.is_outermost(), "inner is nested");
            }
        }
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) == h0 + 1,
              "nested pair → holds_total +1 (outermost only)");
    }

    std::println("\n=== test_guard_enter_ts_optional_1764: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

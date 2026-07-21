// @category: unit
// @reason: Issue #1767 — MutationBoundaryGuard move must transfer
// Issue #1767 (#1978 renamed): issue# moved from filename to header.
// full ownership (enter_ts_ / is_outermost_ / flags) and keep depth
// balanced (moved-from dtor no-ops; moved-to decrements once).
//
//   AC1: source cites #1767; move ctor transfers enter_ts_/is_outermost_
//   AC2: move is noexcept; depth +1 under guard, +0 after move+dtor pair
//   AC3: after move, target is_outermost; source is inert-empty
//   AC4: after move+dtor, hold counters still bump (enter_ts transferred)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: move ctor transfers enter_ts_ / is_outermost_ ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1767") != std::string::npos, "cites #1767");
        auto pos = ixx.find("MutationBoundaryGuard(MutationBoundaryGuard&& o) noexcept");
        CHECK(pos != std::string::npos, "move ctor present");
        auto win = ixx.substr(pos, 1200);
        CHECK(win.find("enter_ts_(std::move(o.enter_ts_))") != std::string::npos,
              "moves enter_ts_");
        CHECK(win.find("is_outermost_(o.is_outermost_)") != std::string::npos,
              "transfers is_outermost_");
        CHECK(win.find("inert_(o.inert_)") != std::string::npos, "transfers inert_");
        static_assert(std::is_nothrow_move_constructible_v<Evaluator::MutationBoundaryGuard>,
                      "move ctor must be noexcept");
        static_assert(std::is_nothrow_move_assignable_v<Evaluator::MutationBoundaryGuard>,
                      "move assign must be noexcept");
        CHECK(true, "move is nothrow constructible/assignable");
    }

    // ── AC2/AC3: depth + is_outermost after move ──
    {
        std::println("\n--- AC2/AC3: depth balance + is_outermost after move ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "start depth 0");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g1(ev, &ok);
            CHECK(ok, "g1 acquired");
            CHECK(g1.is_outermost(), "g1 outermost");
            CHECK(ev.mutation_boundary_depth_slot_value() == 1, "depth 1 after g1");
            Evaluator::MutationBoundaryGuard g2(std::move(g1));
            CHECK(g2.is_outermost(), "g2 outermost after move");
            // g1 is moved-from: not outermost, no live ev.
            CHECK(!g1.is_outermost(), "g1 cleared is_outermost");
            CHECK(ev.mutation_boundary_depth_slot_value() == 1, "depth still 1 (not double)");
            // g1 dtor no-ops (ev_ null)
        }
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after g2 dtor");
        CHECK(ok, "ok flag held");
    }

    // ── AC4: hold metrics after move ──
    {
        std::println("\n--- AC4: hold counters after move (enter_ts transferred) ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g1(ev, &ok);
            Evaluator::MutationBoundaryGuard g2(std::move(g1));
            CHECK(g2.is_outermost(), "moved target outermost");
        }
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) == h0 + 1,
              "holds_total +1 after moved guard dtor");
    }

    std::println("\n=== test_guard_move_ownership_1767: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

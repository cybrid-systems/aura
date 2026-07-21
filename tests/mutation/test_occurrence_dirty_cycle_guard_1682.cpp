// @category: unit
// @reason: Issue #1682 — auto_wire_k_occurrence_dirty_for_subtree must
// terminate on cyclic FlatAST children (visited-set guard; parity #1679).
//
//   AC1: A↔B cycle of IfExpr: walker terminates, marks both Ifs once
//   AC2: self-loop IfExpr: terminates, marks once
//   AC3: acyclic If tree: marks all IfExprs
//   AC4: wall time < 1s for cycle cases

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::auto_wire_k_occurrence_dirty_for_subtree;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
using clock = std::chrono::steady_clock;

static FlatAST* workspace(CompilerService& cs) {
    (void)cs.eval("(set-code \"(define seed 1)\")");
    (void)cs.eval("(eval-current)");
    return cs.workspace_flat();
}

} // namespace

int main() {
    CompilerService cs;
    auto* flat = workspace(cs);
    CHECK(flat != nullptr, "workspace_flat");

    // ── AC3: acyclic tree with two IfExpr ──
    {
        std::println("\n--- AC3: acyclic If tree ---");
        auto lit_t = flat->add_literal(1);
        auto lit_e = flat->add_literal(0);
        auto cond = flat->add_literal(1);
        auto inner = flat->add_if(cond, lit_t, lit_e);
        auto outer = flat->add_if(cond, inner, lit_e);
        std::atomic<int> marks{0};
        const auto t0 = clock::now();
        auto_wire_k_occurrence_dirty_for_subtree(
            *flat,
            [&](std::uint32_t /*id*/, bool /*set*/) -> bool {
                marks.fetch_add(1, std::memory_order_relaxed);
                return false;
            },
            outer);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 1000, std::format("acyclic finished in {}ms", ms));
        CHECK(marks.load() == 2, std::format("acyclic marks both Ifs (got {})", marks.load()));
    }

    // ── AC1: A↔B cycle of IfExpr ──
    {
        std::println("\n--- AC1: If A↔B cycle ---");
        auto lit = flat->add_literal(1);
        // Build two IfExpr, then rewire children into a cycle.
        auto a = flat->add_if(lit, lit, lit);
        auto b = flat->add_if(lit, lit, lit);
        // Force cycle: a.child[0] = b, b.child[0] = a (overwrite cond slots)
        flat->set_child(a, 0, b);
        flat->set_child(b, 0, a);
        std::atomic<int> marks{0};
        const auto t0 = clock::now();
        auto_wire_k_occurrence_dirty_for_subtree(
            *flat,
            [&](std::uint32_t /*id*/, bool /*set*/) -> bool {
                marks.fetch_add(1, std::memory_order_relaxed);
                return false;
            },
            a);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 1000, std::format("cycle finished in {}ms", ms));
        // Each If marked at most once
        CHECK(marks.load() == 2, std::format("cycle marks each If once (got {})", marks.load()));
        std::println("  cycle A↔B marks={} in {}ms", marks.load(), ms);
    }

    // ── AC2: self-loop ──
    {
        std::println("\n--- AC2: self-loop If ---");
        auto lit = flat->add_literal(0);
        auto s = flat->add_if(lit, lit, lit);
        flat->set_child(s, 1, s); // then-branch → self
        std::atomic<int> marks{0};
        const auto t0 = clock::now();
        auto_wire_k_occurrence_dirty_for_subtree(
            *flat,
            [&](std::uint32_t /*id*/, bool /*set*/) -> bool {
                marks.fetch_add(1, std::memory_order_relaxed);
                return false;
            },
            s);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 1000, std::format("self-loop finished in {}ms", ms));
        CHECK(marks.load() == 1, std::format("self-loop marks once (got {})", marks.load()));
        std::println("  self-loop marks={} in {}ms", marks.load(), ms);
    }

    // ── AC4 covered by ms checks ──
    CHECK(true, "AC4 hang threshold embedded");

    std::println("\n=== test_occurrence_dirty_cycle_guard_1682: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

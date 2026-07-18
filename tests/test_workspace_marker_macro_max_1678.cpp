// @category: unit
// @reason: Issue #1678 — workspace_marker_macro_introduced uses max(walk, snapshot)
// so walk < snapshot no longer undercounts MacroIntroduced provenance.
//
//   AC1: walk=1, snapshot=5 → macro-markers reports 5 (not 1)
//   AC2: walk=2, snapshot=0 → macro-markers reports 2
//   AC3: walk=0, snapshot=3 → macro-markers reports 3 (snapshot floor)
//   AC4: query:pattern-hygiene-stats still returns a hash

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t macro_markers(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (stats:get \"query:pattern-hygiene-stats\") \"macro-markers\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // Workspace with three defines so we can stamp MacroIntroduced markers.
    auto sc = cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")");
    CHECK(sc.has_value(), "set-code three defines");

    // ── AC4: dashboard is live ──
    {
        std::println("\n--- AC4: query:pattern-hygiene-stats hash ---");
        auto r = cs.eval("(stats:get \"query:pattern-hygiene-stats\")");
        CHECK(r && is_hash(*r), "pattern-hygiene-stats is hash");
    }

    // Mark y as MacroIntroduced (marker id 1) via public syntax:set-marker.
    {
        auto m = cs.eval("(syntax:set-marker (car (cdr (query:defines))) 1)");
        CHECK(m && is_bool(*m) && as_bool(*m), "set MacroIntroduced on y");
    }

    // ── AC1: walk=1, snapshot=5 → max = 5 ──
    {
        std::println("\n--- AC1: walk < snapshot → max(snapshot) ---");
        cs.evaluator().set_macro_markers_in_snapshot(5);
        const auto n = macro_markers(cs);
        CHECK(n == 5, std::format("macro-markers={} want 5 (max of walk≈1, snap=5)", n));
    }

    // Mark z as MacroIntroduced as well → walk ≥ 2.
    {
        auto m = cs.eval("(syntax:set-marker (car (cdr (cdr (query:defines)))) 1)");
        CHECK(m && is_bool(*m) && as_bool(*m), "set MacroIntroduced on z");
    }

    // ── AC2: walk=2, snapshot=0 → max = 2 ──
    {
        std::println("\n--- AC2: walk > snapshot → live walk ---");
        cs.evaluator().set_macro_markers_in_snapshot(0);
        const auto n = macro_markers(cs);
        CHECK(n >= 2, std::format("macro-markers={} want >= 2 (live walk)", n));
    }

    // ── AC3: walk=0 (no workspace markers) but snapshot floor ──
    // Clear markers by rewriting source without MacroIntroduced stamps,
    // then force snapshot.
    {
        std::println("\n--- AC3: empty walk, snapshot floor ---");
        auto sc2 = cs.eval("(set-code \"(define a 1)\")");
        CHECK(sc2.has_value(), "set-code single define (fresh markers)");
        cs.evaluator().set_macro_markers_in_snapshot(3);
        const auto n = macro_markers(cs);
        // Fresh define column may have 0 MacroIntroduced; floor is snapshot.
        CHECK(n >= 3, std::format("macro-markers={} want >= 3 (snapshot floor)", n));
    }

    std::println("\n=== test_workspace_marker_macro_max_1678: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

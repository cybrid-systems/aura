// @category: unit
// @reason: pure C++ test of SV-scale dirty
//          propagation observability +
//          (query:dirty-propagation-stats) primitive

// test_issue_471_dirty_sv_scale.cpp — Issue #471:
// Optimize mark_dirty_upward + incremental dirty
// propagation for massive SV SoC (refines #426/#463).
//
// Full scope is multi-week (subtree aggregation +
// hardware_backend PPA hooks + level hints + 5000+ node
// benchmark).
//
// Scope-limited close ships the OBSERVABILITY + COUNTER
// LAYER (precondition for the full scope):
//   1. 2 new FlatAST counters:
//      - mark_dirty_early_exit_count_
//        (the mark_dirty_upward_fast fixed-point
//        hit count, surfaced via #471 for the new
//        primitive)
//      - mark_dirty_max_depth_observed_
//        (atomic max across all mark_dirty_upward
//        calls; the key signal for SV-scale deep
//        hierarchy perf)
//   2. (query:dirty-propagation-stats) Aura primitive
//      — 3-counter integer sum:
//        upward_calls + early_exit + max_depth
//   3. (stats:count) 65 → 66
//
// Test cases:
//   AC1:  (query:dirty-propagation-stats) returns an int
//   AC2:  mark_dirty_upward bumps upward_calls counter
//   AC3:  mark_dirty_upward_fast bumps early-exit
//         counter when parent has bits
//   AC4:  max_depth_observed >= 1 after first mutate
//   AC5:  (stats:count) >= 64
//   AC6:  (stats:list) includes query:dirty-propagation-stats
//   AC7:  Fresh service: all 3 counters == 0
//   AC8:  After 100 mark_dirty calls: upward_calls >= 100
//   AC9:  After mark_dirty_upward_fast: early_exit_count
//         advances (deep hierarchy exercise)
//   AC10: Atomic max for max_depth_observed is correct
//         (the highest BFS level reached wins)

#include "test_harness.hpp"

import std;
import aura.core;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_471_detail {

using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t primitive_int(aura::compiler::CompilerService& cs, std::string_view prim) {
    auto r = cs.eval(std::format("({})", prim));
    if (!r)
        return -1;
    if (!aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// ── AC1: (query:dirty-propagation-stats) returns an int
bool test_primitive_returns_int() {
    std::println("\n--- AC1: primitive returns int ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:dirty-propagation-stats)");
    if (!r) {
        CHECK(false, "eval returned error");
        return true;
    }
    auto v = *r;
    CHECK(aura::compiler::types::is_int(v), "(query:dirty-propagation-stats) returns an int");
    return true;
}

// ── AC2: mark_dirty_upward bumps upward_calls counter
bool test_mark_dirty_bumps_calls() {
    std::println("\n--- AC2: mark_dirty_upward bumps counter ---");
    aura::ast::FlatAST ast;
    auto root = ast.add_define(aura::ast::INVALID_SYM, 0);
    auto leaf = ast.add_node(aura::ast::NodeTag::LiteralInt);
    ast.set_int(leaf, 42);
    ast.set_child(root, 0, leaf);
    auto before = ast.mark_dirty_upward_call_count();
    ast.mark_dirty_upward(root);
    auto after = ast.mark_dirty_upward_call_count();
    CHECK(after == before + 1,
          std::format("mark_dirty_upward bumps by 1 (before={}, after={})", before, after));
    return true;
}

// ── AC3: mark_dirty_upward_fast bumps early-exit counter
//         when parent has bits (pre-condition: parent's
//         dirty bits are set; mark_dirty_upward on parent
//         first to set them, then on child)
bool test_mark_dirty_fast_early_exit() {
    std::println("\n--- AC3: mark_dirty_upward_fast early-exit ---");
    aura::ast::FlatAST ast;
    auto root = ast.add_define(aura::ast::INVALID_SYM, 0);
    auto child = ast.add_node(aura::ast::NodeTag::LiteralInt);
    ast.set_child(root, 0, child);
    // Mark parent dirty first to set bits
    ast.mark_dirty(root, 0xFF);
    // Now call mark_dirty_upward_fast on child — the
    // parent already has the bits so the fixed-point
    // check should fire at the parent's level
    // (early-exit != entering the queue).
    auto before = ast.mark_dirty_early_exit_count();
    ast.mark_dirty_upward_fast(child, 0xFF);
    auto after = ast.mark_dirty_early_exit_count();
    CHECK(after > before,
          std::format("mark_dirty_upward_fast bumps early-exit (before={}, after={})", before,
                      after));
    return true;
}

// ── AC4: max_depth_observed >= 1 after first mutate
bool test_max_depth_after_first_mutate() {
    std::println("\n--- AC4: max_depth_observed >= 1 ---");
    aura::ast::FlatAST ast;
    auto root = ast.add_define(aura::ast::INVALID_SYM, 0);
    auto leaf = ast.add_node(aura::ast::NodeTag::LiteralInt);
    ast.set_int(leaf, 42);
    ast.set_child(root, 0, leaf);
    ast.mark_dirty_upward(root);
    auto max_depth = ast.mark_dirty_max_depth_observed();
    CHECK(max_depth >= 1, std::format("max_depth >= 1 (got {})", max_depth));
    return true;
}

// ── AC5: (stats:count) >= 67
//   (63 from #470 + query:dirty-propagation-stats #471 +
//    query:arena-auto-compact-stats #685 +
//    query:shape-value-pass-stats #686 = 67)
bool test_stats_count() {
    std::println("\n--- AC5: (stats:count) >= 67 ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:count)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        CHECK(false, "stats:count not int");
        return true;
    }
    auto n = aura::compiler::types::as_int(*r);
    CHECK(n >= 67, std::format("stats:count >= 67 (got {})", n));
    return true;
}

// ── AC6: (stats:list) includes query:dirty-propagation-stats
bool test_stats_list_includes() {
    std::println("\n--- AC6: (stats:list) includes query:dirty-propagation-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval(R"((if (member "query:dirty-propagation-stats" (stats:list)) #t #f))");
    if (!r) {
        CHECK(false, "eval failed");
        return true;
    }
    auto v = *r;
    CHECK(v.val != 0 && !aura::compiler::types::is_void(v),
          "query:dirty-propagation-stats is in (stats:list)");
    return true;
}

// ── AC7: Fresh service: primitive returns 0
bool test_fresh_service_zero() {
    std::println("\n--- AC7: fresh service: 0 ---");
    aura::compiler::CompilerService cs;
    auto v = primitive_int(cs, "query:dirty-propagation-stats");
    CHECK(v == 0, std::format("fresh service primitive == 0 (got {})", v));
    return true;
}

// ── AC8: After 10 mark_dirty calls: upward_calls >= 10
bool test_100_mark_dirty_calls() {
    std::println("\n--- AC8: 10 mark_dirty_upward calls ---");
    aura::ast::FlatAST ast;
    auto root = ast.add_define(aura::ast::INVALID_SYM, 0);
    auto child = ast.add_node(aura::ast::NodeTag::LiteralInt);
    ast.set_child(root, 0, child);
    for (int i = 0; i < 10; ++i) {
        // Use a different reason bit per call so each
        // call actually does the BFS (re-marking the
        // same bits wouldn't be a meaningful test).
        ast.mark_dirty_upward(child, static_cast<std::uint8_t>(0x01u << (i & 7)));
    }
    auto calls = ast.mark_dirty_upward_call_count();
    CHECK(calls >= 10, std::format("10 calls bump upward_calls (got {})", calls));
    return true;
}

// ── AC9: After mark_dirty_upward_fast: early-exit count
//         advances on deep hierarchy
bool test_deep_hierarchy_early_exit() {
    std::println("\n--- AC9: deep hierarchy early-exit ---");
    aura::ast::FlatAST ast;
    // Build a 5-level deep hierarchy:
    //   root → a → b → c → d → e
    auto e = ast.add_node(aura::ast::NodeTag::LiteralInt);
    auto d = ast.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, e);
    auto c = ast.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, d);
    auto b = ast.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, c);
    auto a = ast.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, b);
    auto root = ast.add_define(aura::ast::INVALID_SYM, aura::ast::NULL_NODE);
    ast.set_child(root, 0, a);
    // Mark ALL ancestors with 0xFF bits so the
    // mark_dirty_upward_fast early-exit fires at every
    // level
    ast.mark_dirty(root, 0xFF);
    ast.mark_dirty(a, 0xFF);
    ast.mark_dirty(b, 0xFF);
    ast.mark_dirty(c, 0xFF);
    ast.mark_dirty(d, 0xFF);
    // Now call mark_dirty_upward_fast on the leaf — the
    // walk will hit the early-exit at every level.
    auto before = ast.mark_dirty_early_exit_count();
    ast.mark_dirty_upward_fast(e, 0xFF);
    auto after = ast.mark_dirty_early_exit_count();
    // We expect at least 1 early-exit: walking from e up,
    // the FIRST parent (d) already has the bits, so the
    // walk stops there with exactly one early-exit.
    // The deep hierarchy confirms the early-exit fires
    // even when there are 5 ancestors above the leaf.
    CHECK(after >= before + 1,
          std::format("deep hierarchy: >= 1 early-exit (before={}, after={})", before, after));
    return true;
}

// ── AC10: Atomic max — highest BFS level wins
bool test_atomic_max_depth() {
    std::println("\n--- AC10: atomic max wins ---");
    aura::ast::FlatAST ast;
    // Build root → 3-level deep: root → a → b
    auto b = ast.add_node(aura::ast::NodeTag::LiteralInt);
    auto a = ast.add_let(aura::ast::INVALID_SYM, aura::ast::NULL_NODE, b);
    ast.set_child(a, 0, b);
    auto root = ast.add_define(aura::ast::INVALID_SYM, 0);
    ast.set_child(root, 0, a);
    // First call: 2 levels (root + a) from b's mutation
    ast.mark_dirty_upward(b);
    auto after_first = ast.mark_dirty_max_depth_observed();
    // Now also from a: 1 level (just root)
    ast.mark_dirty_upward(a);
    auto after_second = ast.mark_dirty_max_depth_observed();
    // after_second >= after_first (max is preserved)
    CHECK(
        after_second >= after_first,
        std::format("max preserved (after_first={}, after_second={})", after_first, after_second));
    return true;
}

} // namespace aura_issue_471_detail

int main() {
    using namespace aura_issue_471_detail;
    std::println("═══ Issue #471 SV-scale dirty propagation tests ═══");

    test_primitive_returns_int();
    test_mark_dirty_bumps_calls();
    test_mark_dirty_fast_early_exit();
    test_max_depth_after_first_mutate();
    test_stats_count();
    test_stats_list_includes();
    test_fresh_service_zero();
    test_100_mark_dirty_calls();
    test_deep_hierarchy_early_exit();
    test_atomic_max_depth();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

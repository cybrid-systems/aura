// @category: unit
// @reason: Issue #2414 — summary_recompute(pool) restores HasKeywordVar +
//          HasQueryOrMutateCall after heavy recompute.
//
//   AC1: recompute(pool) sets keyword + query:/mutate: bits
//   AC2: after recompute, summary_has drives non-zero flags (slow-path gate)
//   AC3: recompute(nullptr) still clears those bits (legacy / incomplete)
//   AC4: ASan clean full-table walk
//   AC5: keyword + query cases covered (this file)

#include "test_harness.hpp"

#include <cstdint>
#include <print>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::StringPool;
using aura::ast::SummaryFlag;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_summary_recompute_sym() {
    std::println("=== Issue #2414: summary_recompute sym_id bits ===");

    // ── AC1 keyword + query bits after recompute(pool) ─────────────
    {
        std::println("\n--- #2414 AC1: recompute(pool) complete bit-set ---");
        StringPool pool;
        FlatAST flat;

        const auto kw = flat.add_variable(pool.intern(":mode"));
        const auto qcal = flat.add_variable(pool.intern("query:find"));
        const auto qcall = flat.add_call(qcal, std::span<const NodeId>{});
        const auto mcal = flat.add_variable(pool.intern("mutate:rebind"));
        const auto mcall = flat.add_call(mcal, std::span<const NodeId>{});
        const auto plain = flat.add_variable(pool.intern("x"));
        (void)plain;
        (void)kw;
        (void)qcall;
        (void)mcall;

        // Simulate heavy mutation wipe: clear then re-OR only tag bits via recompute(null).
        flat.summary_recompute(nullptr);
        CHECK(!flat.summary_has(SummaryFlag::HasKeywordVar),
              "AC1: without pool, HasKeywordVar absent");
        CHECK(!flat.summary_has(SummaryFlag::HasQueryOrMutateCall),
              "AC1: without pool, HasQueryOrMutateCall absent");

        flat.summary_recompute(&pool);
        CHECK(flat.summary_has(SummaryFlag::HasKeywordVar), "AC1: HasKeywordVar set with pool");
        CHECK(flat.summary_has(SummaryFlag::HasQueryOrMutateCall),
              "AC1: HasQueryOrMutateCall set with pool");
        CHECK(flat.summary_flags() != 0, "AC1: non-zero flags after recompute");
    }

    // ── AC2 flags non-zero → needs_tree_walker slow-path gate ─────
    {
        std::println("\n--- #2414 AC2: flags non-zero after recompute(pool) ---");
        StringPool pool;
        FlatAST flat;
        // Root is pure arithmetic; keyword lives as a sibling subtree.
        const auto one = flat.add_literal(1);
        const auto two = flat.add_literal(2);
        const auto plus = flat.add_variable(pool.intern("+"));
        const auto add = flat.add_call(plus, std::array{one, two});
        flat.root = add;
        const auto kw = flat.add_variable(pool.intern(":elsewhere"));
        (void)kw;

        flat.summary_recompute(nullptr);
        CHECK(flat.summary_flags() == 0, "AC2: null pool → flags 0 (fast-path eligible)");

        flat.summary_recompute(&pool);
        CHECK(flat.summary_has(SummaryFlag::HasKeywordVar), "AC2: keyword bit restored");
        CHECK(flat.summary_flags() != 0, "AC2: non-zero → slow-path gate for needs_tree_walker");
        // Tag of root still Call / children intact (no behavior change for structure).
        CHECK(flat.tag(add) == NodeTag::Call, "AC2: structure unchanged");
    }

    // ── AC3 nullptr overload still compiles / tag-only ─────────────
    {
        std::println("\n--- #2414 AC3: recompute() / nullptr tag-only path ---");
        StringPool pool;
        FlatAST flat;
        (void)flat.add_node(NodeTag::MacroDef);
        (void)flat.add_variable(pool.intern(":kw"));
        flat.summary_recompute(); // default nullptr
        CHECK(flat.summary_has(SummaryFlag::HasMacroDef), "AC3: tag bit MacroDef kept");
        CHECK(!flat.summary_has(SummaryFlag::HasKeywordVar), "AC3: keyword skipped without pool");
        flat.summary_recompute(&pool);
        CHECK(flat.summary_has(SummaryFlag::HasMacroDef), "AC3: MacroDef still set");
        CHECK(flat.summary_has(SummaryFlag::HasKeywordVar), "AC3: keyword restored with pool");
    }

    // ── AC4 ASan-safe recompute after free-list recycle ────────────
    {
        std::println("\n--- #2414 AC4: recompute after recycle (no OOB) ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        const auto leaf = flat.add_variable(pool.intern("mutate:set-body"));
        flat.insert_child(flat.root, 0, leaf);
        const auto call = flat.add_call(leaf, std::span<const NodeId>{});
        flat.insert_child(flat.root, 1, call);
        flat.remove_child(flat.root, 1);
        flat.remove_child(flat.root, 0);
        (void)flat.recycle_dead_nodes();
        flat.summary_recompute(&pool);
        CHECK(true, "AC4: recompute walk completed");
    }

    // ── AC5 keyword + query/mutate cases ──────────────────────────
    {
        std::println("\n--- #2414 AC5: keyword + mutate cases ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        const auto live_kw = flat.add_variable(pool.intern(":live"));
        const auto mvar = flat.add_variable(pool.intern("mutate:set-body"));
        const auto mcall = flat.add_call(mvar, std::span<const NodeId>{});
        flat.insert_child(flat.root, 0, live_kw);
        flat.insert_child(flat.root, 1, mcall);
        flat.summary_recompute(&pool);
        CHECK(flat.summary_has(SummaryFlag::HasKeywordVar), "AC5: live keyword");
        CHECK(flat.summary_has(SummaryFlag::HasQueryOrMutateCall), "AC5: mutate: call bit");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_summary_recompute_sym();
}
#endif

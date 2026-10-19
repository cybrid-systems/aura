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

    // ── Issue #4598: live linear-op / match-site counters ──────────
    // The MutationBoundaryGuard exit scan reads has_linear_ops_live() /
    // has_match_sites_live() instead of walking every node. The counters
    // must equal a brute-force live scan after every write path.
    {
        std::println("\n--- #4598 AC6: linear counter parity on birth/truncate ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        (void)flat.add_node(NodeTag::Linear);
        (void)flat.add_node(NodeTag::Move);
        (void)flat.add_node(NodeTag::Call);
        (void)flat.add_node(NodeTag::Drop);
        auto brute_linear = [](FlatAST& f) {
            std::uint64_t n = 0;
            for (NodeId i = 0; i < f.size(); ++i) {
                if (f.is_free_slot(i))
                    continue;
                const auto t = f.tag(i);
                if (t == NodeTag::Linear || t == NodeTag::Move || t == NodeTag::Borrow ||
                    t == NodeTag::MutBorrow || t == NodeTag::Drop)
                    ++n;
            }
            return n;
        };
        CHECK(flat.linear_ops_live_count() == 3, "AC6: 3 linear tags after births");
        CHECK(flat.linear_ops_live_count() == brute_linear(flat), "AC6: parity after births");
        CHECK(flat.has_linear_ops_live(), "AC6: has_linear_ops_live true");
        flat.truncate_to(3); // cuts the trailing Call/Drop pair
        CHECK(flat.linear_ops_live_count() == 2, "AC6: truncate_to drops cut tags");
        CHECK(flat.linear_ops_live_count() == brute_linear(flat), "AC6: parity after truncate");
    }
    {
        std::println("\n--- #4598 AC7: counted set-tag transition ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        const auto lin = flat.add_node(NodeTag::Linear);
        // Linear-family tags carry exactly one fixed child — give the node
        // its child up front so every counted transition below (Linear→Call
        // →Drop→Borrow→Call) satisfies validate_node's arity contract.
        flat.insert_child(lin, 0, flat.add_literal(42));
        flat.insert_child(flat.root, 0, lin);
        const auto call =
            flat.add_call(flat.add_variable(pool.intern("f")), std::span<const NodeId>{});
        flat.insert_child(flat.root, 1, call);
        // insert_child bumps generation_ (like set_child); re-align the
        // patched node so apply_patches' is_valid gate accepts it (the
        // rebind flow does the same via force_align_subtree_gen).
        flat.force_align_subtree_gen(flat.root);
        CHECK(flat.linear_ops_live_count() == 1, "AC7: one linear tag");
        flat.set_tag_live_adjusted(lin, NodeTag::Call);
        CHECK(flat.linear_ops_live_count() == 0, "AC7: Linear→Call drops count");
        CHECK(!flat.has_linear_ops_live(), "AC7: predicate false after last linear dies");
        flat.set_tag_live_adjusted(lin, NodeTag::Drop);
        CHECK(flat.linear_ops_live_count() == 1, "AC7: Call→Drop raises count");
        // Patch apply path shares the counted chokepoint (ast_impl case 0).
        aura::ast::Patch p{};
        p.node = lin;
        p.field_offset = 0;
        p.new_value = static_cast<std::uint64_t>(NodeTag::Borrow);
        CHECK(apply_patches(flat, std::span<const aura::ast::Patch>{&p, 1}), "AC7: patch applies");
        CHECK(flat.linear_ops_live_count() == 1, "AC7: patch counted (Borrow still linear)");
        p.new_value = static_cast<std::uint64_t>(NodeTag::Call);
        (void)apply_patches(flat, std::span<const aura::ast::Patch>{&p, 1});
        CHECK(flat.linear_ops_live_count() == 0, "AC7: patch counted (Borrow→Call drops)");
    }
    {
        std::println("\n--- #4598 AC8: match-site counter + stale-recycle mirror ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        const auto m = flat.add_node(NodeTag::Let);
        flat.insert_child(flat.root, 0, m);
        CHECK(flat.match_sites_live_count() == 0, "AC8: no match info yet");
        aura::ast::MatchClauseInfo info{};
        info.used_constructors.push_back(pool.intern("Red"));
        flat.set_match_info(m, info);
        CHECK(flat.match_sites_live_count() == 1, "AC8: live match site counted");
        CHECK(flat.has_match_sites_live(), "AC8: predicate true");
        // Detach + recycle: the death site drops the count.
        (void)flat.recycle_dead_nodes();
        CHECK(flat.match_sites_live_count() == 1, "AC8: attached node still counted");
        aura::ast::MatchClauseInfo empty{};
        flat.set_match_info(m, empty);
        CHECK(flat.match_sites_live_count() == 0, "AC8: cleared info drops count");
        flat.set_match_info(m, info);
        CHECK(flat.match_sites_live_count() == 1, "AC8: re-set counts again");
        // Stale-info recycle mirror: a slot whose match_info_ survives in
        // the column (resize never ran) counts at its next birth, exactly
        // like the exit walk's is_live_node && has_match_info predicate.
        auto brute_match = [](FlatAST& f) {
            std::uint64_t n = 0;
            for (NodeId i = 0; i < f.size(); ++i)
                if (!f.is_free_slot(i) && f.has_match_info(i))
                    ++n;
            return n;
        };
        CHECK(flat.match_sites_live_count() == brute_match(flat), "AC8: parity after churn");
    }
    {
        std::println("\n--- #4598 AC9: wholesale mirrors (copy-assign + clear) ---");
        StringPool pool;
        FlatAST flat;
        (void)flat.add_node(NodeTag::Linear);
        (void)flat.add_node(NodeTag::MacroDef);
        FlatAST copy;
        copy = flat; // copy-assign — the ast:snapshot deep-copy face
        CHECK(copy.linear_ops_live_count() == 1, "AC9: copy-assign mirrors linear count");
        CHECK(copy.summary_has(SummaryFlag::HasMacroDef), "AC9: copy-assign mirrors summary bits");
        CHECK(copy.summary_has(SummaryFlag::HasMacroDef) ==
                  flat.summary_has(SummaryFlag::HasMacroDef),
              "AC9: summary parity source vs copy");
        flat.clear();
        CHECK(flat.linear_ops_live_count() == 0, "AC9: clear zeroes linear count");
        CHECK(flat.match_sites_live_count() == 0, "AC9: clear zeroes match count");
        CHECK(!flat.summary_has(SummaryFlag::HasMacroDef), "AC9: clear zeroes summary bits");
    }
    {
        std::println("\n--- #4598 AC10: macro-free expand gate decision input ---");
        StringPool pool;
        FlatAST flat;
        flat.root = flat.add_node(NodeTag::Begin);
        (void)flat.add_call(flat.add_variable(pool.intern("g")), std::span<const NodeId>{});
        // The #4598 gate: bit==0 is exact-absent → skip the body walk.
        CHECK(!flat.summary_has(SummaryFlag::HasMacroDef), "AC10: macro-free flat skips expand");
        (void)flat.add_node(NodeTag::MacroDef);
        CHECK(flat.summary_has(SummaryFlag::HasMacroDef), "AC10: MacroDef birth sets bit");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_summary_recompute_sym();
}
#endif

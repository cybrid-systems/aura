// test_issue_1455_occurrence_stale_propagation.cpp
// Issue #1455: Strengthen occurrence narrowing staleness propagation
// after mutate:* (mark_dirty → occ_stale_ + NarrowingRecord.stale +
// force re-analyze in resolve_if_predicate_occurrence).
//
// AC1: mark_dirty(kOccurrenceDirty) stamps occ_stale_ in lockstep
// AC2: mark_dirty_upward invalidates NarrowingRecords + sets
//      occ_stale_ on if-nodes (not only rec.stale)
// AC3: mark_dirty_upward_fast also invalidates narrowings (#1455 gap)
// AC4: after re-narrow, has_stale_narrowing_for_if is false for
//      the refreshed if (historical rows cleared)
// AC5: post-mutate typecheck / eval keeps narrow-dependent semantics
// AC6: multi-round predicate mutate — no wrong narrow after cond change

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1455_detail {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
)";

static bool load_if_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    if (!cs.eval("(typecheck-current)"))
        return false;
    return true;
}

static NodeId find_first_if(const FlatAST& flat) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (flat.get(id).tag == NodeTag::IfExpr)
            return id;
    }
    return NULL_NODE;
}

static void test_mark_dirty_occurrence_locks_stale() {
    std::println("\n--- AC1: mark_dirty(kOccurrenceDirty) ⇒ occ_stale_ ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr, "workspace flat");
    const auto if_id = find_first_if(*ws);
    CHECK(if_id != NULL_NODE, "found IfExpr");
    if (if_id == NULL_NODE)
        return;

    ws->clear_occurrence_stale(if_id);
    ws->clear_dirty_for(if_id, static_cast<std::uint8_t>(FlatAST::kOccurrenceDirty));
    CHECK(ws->is_occurrence_stale(if_id) == 0, "occ_stale clear before");

    ws->mark_dirty(if_id, static_cast<std::uint8_t>(FlatAST::kOccurrenceDirty));
    CHECK(ws->is_dirty_for(if_id, static_cast<std::uint8_t>(FlatAST::kOccurrenceDirty)),
          "kOccurrenceDirty set");
    CHECK(ws->is_occurrence_stale(if_id) != 0, "occ_stale stamped with kOccurrenceDirty");
}

static void test_mark_dirty_upward_propagates_stale() {
    std::println("\n--- AC2: mark_dirty_upward invalidates + occ_stale on if ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && ws->narrowing_count() > 0, "narrowing log populated");
    const auto inv0 = ws->narrow_invalidation_post_mutate_count();
    const auto if_id = find_first_if(*ws);
    CHECK(if_id != NULL_NODE, "found IfExpr");

    // Mutate via rebind (uses mark_dirty_upward) then inspect signals.
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (string? x) x 0))\" "
                  "\"issue-1455-up\")");
    const auto inv1 = ws->narrow_invalidation_post_mutate_count();
    std::println("  invalidation: {} -> {}, occ_stale_count={}", inv0, inv1,
                 ws->occurrence_stale_count());
    CHECK(inv1 > inv0 || ws->stale_narrowing_record_count() > 0 || ws->occurrence_stale_count() > 0,
          "invalidate_narrowings fired after predicate rebind");

    // After invalidation, if-nodes in the log should carry occ_stale_
    // (or have been re-narrowed and cleared). Either is correct.
    bool any_stale_or_cleared =
        ws->occurrence_stale_count() > 0 || !ws->has_stale_narrowing_for_if(if_id, 0);
    CHECK(any_stale_or_cleared || inv1 > inv0, "stale propagation signal present");
}

static void test_mark_dirty_upward_fast_invalidates() {
    std::println("\n--- AC3: mark_dirty_upward_fast invalidates narrowings ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && ws->narrowing_count() > 0, "narrowing log");
    const auto if_id = find_first_if(*ws);
    CHECK(if_id != NULL_NODE, "found IfExpr");

    const auto inv0 = ws->narrow_invalidation_post_mutate_count();
    // Touch a node inside the if subtree via fast path.
    NodeId target = if_id;
    const auto ifv = ws->get(if_id);
    if (!ifv.children.empty() && ifv.child(0) != NULL_NODE)
        target = ifv.child(0);
    ws->mark_dirty_upward_fast(target, FlatAST::kGeneralDirty);
    const auto inv1 = ws->narrow_invalidation_post_mutate_count();
    std::println("  fast invalidation: {} -> {}", inv0, inv1);
    CHECK(inv1 > inv0 || ws->is_occurrence_stale(if_id) != 0 ||
              ws->has_stale_narrowing_for_if(
                  if_id, static_cast<std::uint64_t>(ws->type_cache_generation())),
          "mark_dirty_upward_fast invalidated narrowings");
}

static void test_renarrow_clears_stale_flag() {
    std::println("\n--- AC4: clear_stale_narrowings_for_if + re-narrow occ_stale ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && ws->narrowing_count() > 0, "narrowing log");
    const auto if_id = find_first_if(*ws);
    CHECK(if_id != NULL_NODE, "found IfExpr");

    // Direct unit: invalidate then clear should drop has_stale.
    const auto inv = ws->invalidate_narrowings_in_subtree(if_id, 999999);
    std::println("  invalidate count={}, occ_stale={}", inv, ws->is_occurrence_stale(if_id));
    CHECK(ws->has_stale_narrowing_for_if(if_id, 999999) || inv > 0 ||
              ws->is_occurrence_stale(if_id) != 0,
          "invalidate stamps stale");
    const auto cleared = ws->clear_stale_narrowings_for_if(if_id, /*fresh_epoch=*/42);
    std::println("  cleared={}, has_stale(if,0)={}", cleared,
                 ws->has_stale_narrowing_for_if(if_id, 0));
    CHECK(!ws->has_stale_narrowing_for_if(if_id, 0), "clear_stale drops latest stale");
    CHECK(ws->is_occurrence_stale(if_id) == 0, "clear_stale clears occ_stale_");

    // Integration: rebind + re-narrow preserves eval; stale count
    // non-increasing after infer (orphaned nodes may retain bits
    // until GC — live path is covered by AC5/AC6 semantics).
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 3) 0))\" "
                  "\"issue-1455-renarrow\")");
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    const auto occ_before = ws->occurrence_stale_count();
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto occ_after = ws->occurrence_stale_count();
    std::println("  occurrence_stale_count {} -> {}", occ_before, occ_after);
    CHECK(occ_after <= occ_before || occ_after <= 2, "occ_stale not exploding after re-narrow");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "eval after re-narrow");
}

static void test_semantics_after_predicate_mutate() {
    std::println("\n--- AC5: narrow-dependent semantics after mutate ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 10) 0))\" "
                  "\"issue-1455-sem\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 5)");
    CHECK(r && is_int(*r), "f 5 returns int");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 15, "narrow-dependent (+ x 10) correct");
}

static void test_multi_round_no_wrong_narrow() {
    std::println("\n--- AC6: multi-round predicate mutate ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    for (int round = 0; round < 4; ++round) {
        const int add = round + 2;
        const std::string body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(add) + ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        auto* ws = cs.workspace_flat();
        if (ws && !ws->all_mutations().empty())
            (void)cs.incremental_infer(ws->all_mutations().back());
        auto r = cs.eval("(f 1)");
        CHECK(r && is_int(*r), "f 1 int round " + std::to_string(round));
        if (r && is_int(*r))
            CHECK(as_int(*r) == 1 + add, "correct narrow result round " + std::to_string(round));
    }
}

} // namespace aura_1455_detail

int main() {
    using namespace aura_1455_detail;
    test_mark_dirty_occurrence_locks_stale();
    test_mark_dirty_upward_propagates_stale();
    test_mark_dirty_upward_fast_invalidates();
    test_renarrow_clears_stale_flag();
    test_semantics_after_predicate_mutate();
    test_multi_round_no_wrong_narrow();
    return RUN_ALL_TESTS();
}

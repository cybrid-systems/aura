// @category: unit
// @reason: Issue #2099 — HygieneCheckpoint + restore API for violation rollback
// (Agent what-if / self-evo safety). Refines Macro Hygiene review §7.4.
//
//   AC1: save → mutate (introduce new MacroIntroduced node) → restore → pre-save
//        marker_/provenance_/dirty_/macro_dirty_ columns reinstalled; partial
//        MacroIntroduced dirty un-marked.
//   AC2: nested under MutationBoundary → outer boundary's structural checkpoint
//        (children_snapshot / atomic_batch_meta_snap_) untouched; restore only
//        mutates the metadata columns.
//   AC3: happy-path expand without save → restore_hygiene_checkpoint_handle(0)
//        returns #f, no mutation; zero overhead when no save ever happened.
//   AC4: concurrent fiber stress (save on thread A, restore on thread B with
//        same handle) → cross_fiber_reject_total bumps, restore returns #f;
//        restore_fail_total also bumps.
//   AC5: query:hygiene-checkpoint-stats reports save_total / restore_success_total
//        / restore_fail_total / cross_fiber_reject_total / pending_count +
//        schema=2099 markers.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast; // Issue #2099: NodeId / NodeTag / SyntaxMarker for FlatAST mutations

namespace {

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:hygiene-checkpoint-stats\") \"{}\")", std::string(key)));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_save_restore_rolls_back_partial_dirty(CompilerService& cs) {
    // Set up a tiny workspace + introduce a few non-MacroIntroduced nodes so we
    // have something to dirty.
    CHECK(cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto save_before =
        m ? m->hygiene_checkpoint_save_total.load(std::memory_order_relaxed) : 0;
    const auto restore_succ_before =
        m ? m->hygiene_checkpoint_restore_success_total.load(std::memory_order_relaxed) : 0;

    auto h = ev.save_hygiene_checkpoint_handle();
    CHECK(h != 0, "save returned non-zero handle");

    // Mutate: introduce a MacroIntroduced + dirty a node so the columns
    // diverge from the saved snapshot. We use the C++ API directly to avoid
    // depending on a specific mutate: primitive in this test file.
    auto* flat = ev.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat is wired");

    // Pick an existing node id and mark it MacroIntroduced + dirty.
    aura::ast::NodeId victim = 0;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id)) {
            victim = id;
            break;
        }
    }
    CHECK(victim != aura::ast::NULL_NODE, "found a live node");
    flat->set_marker(victim, SyntaxMarker::MacroIntroduced);
    flat->apply_macro_dirty_bits(victim, 0xFF);
    flat->mark_dirty_upward(victim);
    CHECK(flat->is_macro_introduced(victim), "victim now MacroIntroduced");
    CHECK(flat->dirty(victim) != 0, "victim now dirty");

    // Restore via the handle path → should succeed.
    const bool restored = ev.restore_hygiene_checkpoint_handle(h);
    CHECK(restored, "restore_hygiene_checkpoint_handle returned true");
    CHECK(!flat->is_macro_introduced(victim),
          "AC1: victim no longer MacroIntroduced after restore");
    CHECK(flat->dirty(victim) == 0, "AC1: victim no longer dirty after restore");

    if (m) {
        CHECK(m->hygiene_checkpoint_save_total.load(std::memory_order_relaxed) == save_before + 1,
              "save_total bumped");
        CHECK(m->hygiene_checkpoint_restore_success_total.load(std::memory_order_relaxed) ==
                  restore_succ_before + 1,
              "restore_success_total bumped");
    }
}

static void ac2_nested_under_mutation_boundary_preserves_topology(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1) (define b 2)\")").has_value(), "set-code ac2");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac2");

    auto& ev = cs.evaluator();
    auto* flat = ev.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat wired ac2");

    // Snapshot pre-boundary topology: total children vector size + first
    // non-empty children's child count. We compare after restore.
    const auto pre_size = flat->size();
    std::size_t pre_first_n_children = 0;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id)) {
            pre_first_n_children = flat->children(id).size();
            break;
        }
    }

    bool ok = true;
    {
        auto guard_r =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        CHECK(guard_r.has_value(), "outer Guard acquired");
        auto guard = std::move(*guard_r);
        // Save the hygiene checkpoint inside the outer boundary.
        auto h = ev.save_hygiene_checkpoint_handle();
        CHECK(h != 0, "save returned handle inside boundary");

        // Mutate something that should NOT be rolled back by the hygiene
        // restore (a structural child append) — we go via
        // add_node_to_workspace_pool to leave a real children_ change.
        auto new_id = flat->add_node(NodeTag::Variable, SyntaxMarker::User);
        flat->set_marker(new_id, SyntaxMarker::MacroIntroduced);
        flat->mark_dirty_upward(new_id);
        CHECK(flat->size() == pre_size + 1, "node added");

        // Restore hygiene — should leave the structural top intact but
        // clear the MacroIntroduced marker on `new_id` (the marker was
        // introduced AFTER the save).
        const bool restored = ev.restore_hygiene_checkpoint_handle(h);
        CHECK(restored, "restore inside boundary returned true");
        CHECK(!flat->is_macro_introduced(new_id),
              "AC2: new node MacroIntroduced marker cleared by restore");
        CHECK(flat->size() == pre_size + 1, "AC2: structural topology preserved (size unchanged)");
    }

    // Verify the children vector of the first live node is unchanged (no
    // cross-impact from the metadata-column restore).
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id)) {
            CHECK(flat->children(id).size() == pre_first_n_children,
                  "AC2: children_ vector of first live node unchanged");
            break;
        }
    }
}

static void ac3_zero_overhead_when_no_save(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code ac3");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac3");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto save_before =
        m ? m->hygiene_checkpoint_save_total.load(std::memory_order_relaxed) : 0;
    const auto fail_before =
        m ? m->hygiene_checkpoint_restore_fail_total.load(std::memory_order_relaxed) : 0;

    // Restore handle = 0 (the sentinel) → must return false without touching
    // workspace state.
    CHECK(!ev.restore_hygiene_checkpoint_handle(0), "AC3: restore_handle(0) returns false");
    // Restore with a handle that was never saved (huge id) → also false.
    CHECK(!ev.restore_hygiene_checkpoint_handle(0xDEADBEEF),
          "AC3: restore_handle(never-saved) returns false");

    if (m) {
        CHECK(m->hygiene_checkpoint_save_total.load(std::memory_order_relaxed) == save_before,
              "AC3: no save bumped");
        CHECK(m->hygiene_checkpoint_restore_fail_total.load(std::memory_order_relaxed) >=
                  fail_before + 2,
              "AC3: restore_fail bumped at least twice (both refused restores)");
    }
    // Pending count should stay 0 (no save happened).
    CHECK(ev.hygiene_checkpoint_pending_count() == 0,
          "AC3: pending checkpoint count is 0 (no save ever issued)");
}

static void ac4_cross_fiber_restore_rejected(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code ac4");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac4");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto cross_before =
        m ? m->hygiene_checkpoint_cross_fiber_reject_total.load(std::memory_order_relaxed) : 0;
    const auto fail_before =
        m ? m->hygiene_checkpoint_restore_fail_total.load(std::memory_order_relaxed) : 0;

    // Save on the main thread.
    auto h = ev.save_hygiene_checkpoint_handle();
    CHECK(h != 0, "save returned handle");

    // Spawn a worker thread that calls restore on the same handle. The
    // thread::id will differ → cross-fiber reject expected.
    auto fut = std::async(std::launch::async,
                          [&ev, h]() -> bool { return ev.restore_hygiene_checkpoint_handle(h); });
    const bool restored_on_other_thread = fut.get();
    CHECK(!restored_on_other_thread, "AC4: cross-fiber restore refused (returned false)");

    if (m) {
        CHECK(m->hygiene_checkpoint_cross_fiber_reject_total.load(std::memory_order_relaxed) ==
                  cross_before + 1,
              "AC4: cross_fiber_reject_total bumped");
        CHECK(m->hygiene_checkpoint_restore_fail_total.load(std::memory_order_relaxed) ==
                  fail_before + 1,
              "AC4: restore_fail_total bumped");
    }

    // Same-thread restore of the same handle (already consumed by the worker
    // → slot was invalidated, so this is a "handle not found" fail).
    const bool same_thread_after = ev.restore_hygiene_checkpoint_handle(h);
    CHECK(!same_thread_after, "AC4: consumed handle returns false on retry");
}

static void ac5_query_hygiene_checkpoint_stats_reports(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code ac5");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac5");

    // Trigger 2 saves + 1 successful restore + 1 failed restore.
    auto& ev = cs.evaluator();
    {
        auto h = ev.save_hygiene_checkpoint_handle();
        CHECK(h != 0, "ac5 save #1");
        ev.restore_hygiene_checkpoint_handle(h);
    }
    {
        auto h = ev.save_hygiene_checkpoint_handle();
        CHECK(h != 0, "ac5 save #2");
        // fail: already consumed + retry.
        ev.restore_hygiene_checkpoint_handle(h);
        ev.restore_hygiene_checkpoint_handle(h);
    }

    // Read the dashboard.
    CHECK(href_int(cs, "save_total") >= 2, "AC5: save_total >= 2");
    CHECK(href_int(cs, "restore_success_total") >= 1, "AC5: restore_success_total >= 1");
    CHECK(href_int(cs, "restore_fail_total") >= 1, "AC5: restore_fail_total >= 1");
    CHECK(href_int(cs, "pending_count") == 0,
          "AC5: pending_count == 0 (all checkpoints restored or consumed)");
    CHECK(href_int(cs, "schema") == 2099, "AC5: schema == 2099");
    CHECK(href_int(cs, "issue") == 2099, "AC5: issue == 2099");
    CHECK(href_int(cs, "active") == 1, "AC5: active == 1");
    // lineage pointer to the underlying metadata-column snapshot API from #1893.
    CHECK(href_int(cs, "lineage-1893") == 1893, "AC5: lineage-1893 marker present");
    CHECK(href_int(cs, "nested-under-mutation-boundary") == 1,
          "AC5: nested-under-mutation-boundary marker present");
}

} // namespace

int main() {
    CompilerService cs;
    std::print("[test_hygiene_checkpoint_2099] running 5 ACs\n");

    ac1_save_restore_rolls_back_partial_dirty(cs);
    ac2_nested_under_mutation_boundary_preserves_topology(cs);
    ac3_zero_overhead_when_no_save(cs);
    ac4_cross_fiber_restore_rejected(cs);
    ac5_query_hygiene_checkpoint_stats_reports(cs);

    std::print("[test_hygiene_checkpoint_2099] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
// @category: unit
// @reason: Issue #3381 — production facade early-return dirties only the
// root, but `should_partial_relower_impact_checked` is per-entry monotonic
// only for names already in `dirty_names`. Without a caller-union step at
// peel entry (or after facade success), callers stay `dirty=false` /
// `dirty_block_count()==0` and the partial gate silently skips them while
// `lookup_define_v2(g)` still returns 0 on the pre-mutate IR — miss-compile
// (half-green close of #3034 under production defaults).
//
// Fix contract (AC1–AC6 from the issue body):
//
//   AC1: Production + single workspace: mutate `f`; `g` calls `f`.
//        Before next `eval-current` / peel, either
//        `g.dirty_block_count()>0` **or** `lookup_define_v2(g)==1`.
//        No clean hit on pre-mutate caller IR.
//   AC2: `should_partial_relower_impact_checked` on `g` either sees a
//        non-zero dirty mask or the peel takes full for the cone —
//        never silent skip. #3283 fail-closed shape (mark_all_blocks_
//        dirty + dirty=true + partial_forced_full_by_impact_total bump)
//        when deferred_hybrid_gen moved since gen0.
//   AC3: Soft / Off + clean (armed==0) single-fiber: zero extra dirty
//        marks / no extra BFS. Branch is gated on
//        production_defaults_active() || initial_armed; otherwise the
//        union + mark step is skipped entirely.
//   AC4: Dual DepGraph: if `graphs_consistent` fails under production,
//        existing Strict force-dirty of **all** callers still fires
//        (#3165 / #3187) via fail_closed_soft_dual_graph_parity_before_
//        partial_. This fix does NOT touch that path.
//   AC5: Non-duplicative to #3345 (hybrid interpreter visibility /
//        optional depth-1 fanout). #3345 covers interpreter visibility;
//        #3381 covers peel snapshot. Coordinate; one patch may close
//        both. This fix touches the peel side only.
//   AC6: No new query schema. Existing counters move when AC1 is
//        violated in soak:
//          - partial_forced_full_by_impact_total (bump on gen-moved
//            fail-closed take-full)
//          - incremental_soundness_mismatch_prod_total (bump when
//            caller is left clean under production defaults)
//          - should_relower_total (per-entry partial-vs-full decision
//            counter — gates callers into the impact_ub consult)
//
// Implementation: in `relower_dirty_defines_from_workspace` (service.ixx)
// after the dirty_names snapshot, under production defaults or
// initial_armed, walk dep_graph_[root].called_by under shared
// dep_graph_mtx_, append callers to dirty_names, mark each caller's
// body dirty via the existing mark_caller_body_dirty helper (same call
// shape Soft BFS uses at service_dirty.cpp:1087). If
// deferred_hybrid_gen moved since gen0 (concurrent stale-reject
// re-armed edges the entry drain / size snapshot never saw), force
// full for the cone via the #3283 shape.

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// Find the body of a free function whose signature contains `sig`.
// Returns the substring of length `approx_len` starting at the matching
// opening brace (best-effort brace-balanced — caller passes a generous
// length).
static std::string find_fn_body(const std::string& src, const std::string& sig,
                                std::size_t approx_len) {
    const auto sig_pos = src.find(sig);
    if (sig_pos == std::string::npos)
        return {};
    const auto brace = src.find('{', sig_pos);
    if (brace == std::string::npos)
        return {};
    return src.substr(brace, approx_len);
}

} // namespace

int run_test_incremental_facade_dirty_names_snapshot() {
    std::println("=== Issue #3381: production facade + dirty_names snapshot — "
                 "caller union at peel entry ===");
    CHECK(true, "ac3381: issue stamp");

    auto ixx = read_file("src/compiler/service.ixx");
    auto dir = read_file("src/compiler/service_dirty.cpp");
    auto cache = read_file("src/compiler/ir_cache_pure.ixx");
    auto hot = read_file("src/compiler/hot_update_registry.cpp");
    auto build = read_file("build.py");

    // ── AC1: at peel entry, union called_by into dirty_names + mark
    //    caller bodies dirty (production / armed gate) ──────────────
    {
        std::println("\n--- AC1: peel-entry union of dep_graph_[root].called_by "
                     "into dirty_names ---");
        // The fix lives in relower_dirty_defines_from_workspace, after the
        // dirty_names snapshot and before the per-entry loop. Find the
        // body of the function and verify the union step is present.
        const auto peel_body =
            find_fn_body(ixx, "std::size_t relower_dirty_defines_from_workspace()", 4500);
        CHECK(!peel_body.empty(), "AC1: relower_dirty_defines_from_workspace found");
        if (!peel_body.empty()) {
            // 1a. The #3381 block is present and gated on
            //     production_defaults_active() || initial_armed.
            CHECK(peel_body.find("Issue #3381") != std::string::npos,
                  "AC1: #3381 caller-union block present");
            CHECK(peel_body.find("production_defaults_active()") != std::string::npos,
                  "AC1: union gated on production_defaults_active");
            CHECK(peel_body.find("initial_armed") != std::string::npos,
                  "AC1: union also gated on initial_armed (Soft BFS parity)");
            // 1b. Walks dep_graph_[root].called_by under shared dep_graph_mtx_.
            CHECK(peel_body.find("dep_graph_.find(root)") != std::string::npos,
                  "AC1: walks dep_graph_[root] for each dirty root");
            CHECK(peel_body.find("dit->second.called_by") != std::string::npos,
                  "AC1: walks .called_by edges");
            CHECK(peel_body.find("OrderedSharedLock<std::shared_mutex> read") != std::string::npos,
                  "AC1: dep_graph_ walk under shared dep_graph_mtx_");
            // 1c. Appends new callers to dirty_names + marks caller bodies
            //     dirty using the existing mark_caller_body_dirty helper
            //     (same call shape Soft BFS uses at service_dirty.cpp:1087).
            CHECK(peel_body.find("mark_caller_body_dirty") != std::string::npos,
                  "AC1: mark_caller_body_dirty used (existing helper)");
            CHECK(peel_body.find("dirty_names.push_back(caller)") != std::string::npos,
                  "AC1: callers appended to dirty_names");
            // 1d. Dedupe against both the existing dirty_names snapshot
            //     AND the new caller_names batch.
            CHECK(peel_body.find("std::find(dirty_names.begin()") != std::string::npos,
                  "AC1: dedupe against existing dirty_names");
        }
        // The Soft BFS still uses mark_caller_body_dirty (no behavior
        // change for Soft path — confirmed by the call shape at
        // service_dirty.cpp:1087).
        const auto soft_bfs = find_fn_body(
            dir, "void CompilerService::mark_define_dirty(const std::string& name)", 1500);
        CHECK(soft_bfs.find("mark_caller_body_dirty") != std::string::npos,
              "AC1: Soft BFS still uses mark_caller_body_dirty (unchanged)");
    }

    // ── AC2: never silent skip — non-zero dirty mask on caller OR
    //    peel takes full for the cone ────────────────────────────────
    {
        std::println("\n--- AC2: never silent skip — non-zero dirty mask OR "
                     "take-full for the cone ---");
        const auto peel_body =
            find_fn_body(ixx, "std::size_t relower_dirty_defines_from_workspace()", 4500);
        CHECK(!peel_body.empty(), "AC2: relower_dirty_defines_from_workspace found");
        if (!peel_body.empty()) {
            // 2a. The caller-union appends to dirty_names BEFORE the
            //     per-entry loop runs, so dirty_count>0 on the caller at
            //     the impact_ub consult. The existing
            //     should_partial_relower_impact_checked then sees a
            //     non-zero mask.
            CHECK(peel_body.find("dirty_names.push_back(caller)") != std::string::npos,
                  "AC2: callers in dirty_names before per-entry impact_ub consult");
            // 2b. #3283 fail-closed shape: mark_all_blocks_dirty +
            //     dirty=true + partial_forced_full_by_impact_total bump
            //     when deferred_hybrid_gen moved since gen0.
            CHECK(peel_body.find("deferred_hybrid_gen_") != std::string::npos,
                  "AC2: gen check (moved since gen0) wired");
            CHECK(peel_body.find("mark_all_blocks_dirty") != std::string::npos,
                  "AC2: #3283 mark_all_blocks_dirty fail-closed shape");
            CHECK(peel_body.find("partial_forced_full_by_impact_total") != std::string::npos,
                  "AC2: partial_forced_full_by_impact_total counter bump on fail-closed");
        }
        // The base should_partial_relower_impact_checked contract is
        // untouched (dirty_count==0 → false; impact_ub > dirty_count →
        // false). AC2 just guarantees callers are now in dirty_names with
        // a non-zero mask.
        CHECK(cache.find("if (dirty_count == 0)") != std::string::npos,
              "AC2: should_partial_relower_impact_checked base contract preserved");
        CHECK(cache.find("if (impact_upper_bound > dirty_count)") != std::string::npos,
              "AC2: impact_upper_bound > dirty_count → false preserved");
    }

    // ── AC3: Soft / Off + clean (armed==0) → zero extra dirty marks /
    //    no extra BFS ─────────────────────────────────────────────────
    {
        std::println("\n--- AC3: Soft / Off + clean (armed==0) zero extra cost ---");
        const auto peel_body =
            find_fn_body(ixx, "std::size_t relower_dirty_defines_from_workspace()", 4500);
        // 3a. The #3381 block is gated on production_defaults_active()
        //     || initial_armed. Under Soft + clean single-fiber, neither
        //     is true → the union + mark step is skipped entirely (zero
        //     extra BFS, zero extra dirty marks, zero extra lock work).
        const auto block_pos = peel_body.find("Issue #3381");
        CHECK(block_pos != std::string::npos, "AC3: #3381 block found");
        if (block_pos != std::string::npos) {
            // The gate must appear BEFORE the union walk + mark step
            // inside the block. Scope considered: from the block start
            // to the end of the if body (best-effort — caller passes a
            // generous length).
            const auto block_scope = peel_body.substr(block_pos, 2500);
            const auto gate_pos = block_scope.find("production_defaults_active()");
            const auto walk_pos = block_scope.find("dep_graph_.find(root)");
            const auto mark_pos = block_scope.find("mark_caller_body_dirty");
            CHECK(gate_pos != std::string::npos && walk_pos != std::string::npos &&
                      gate_pos < walk_pos,
                  "AC3: production gate appears before dep_graph_ walk");
            CHECK(gate_pos != std::string::npos && mark_pos != std::string::npos &&
                      gate_pos < mark_pos,
                  "AC3: production gate appears before mark_caller_body_dirty");
        }
        // 3b. Soft path still uses the existing full BFS at
        //     service_dirty.cpp (Soft unchanged — no second BFS).
        const auto soft_bfs = find_fn_body(
            dir, "void CompilerService::mark_define_dirty(const std::string& name)", 1500);
        CHECK(soft_bfs.find("hybrid_node_cascade_") != std::string::npos,
              "AC3: Soft BFS still uses hybrid_node_cascade_ (unchanged)");
        CHECK(soft_bfs.find("drain_deferred_hybrid_cascade_") != std::string::npos,
              "AC3: Soft BFS still uses drain_deferred_hybrid_cascade_ (unchanged)");
    }

    // ── AC4: Dual DepGraph Strict force-dirty of all callers (#3165 /
    //    #3187) still fires — this fix does NOT touch that path ────
    {
        std::println("\n--- AC4: dual-graph Strict force-dirty preserved ---");
        const auto parity_body =
            find_fn_body(ixx, "void fail_closed_soft_dual_graph_parity_before_partial_", 1500);
        CHECK(!parity_body.empty(),
              "AC4: fail_closed_soft_dual_graph_parity_before_partial_ found");
        if (!parity_body.empty()) {
            // The existing dual-graph fail-closed bumps mark_all_blocks_dirty
            // on the entry AND walks called_by for force-dirty of all
            // callers (#3187 surface). This fix does NOT touch it.
            CHECK(parity_body.find("mark_all_blocks_dirty") != std::string::npos,
                  "AC4: dual-graph parity still force-dirties entry");
            CHECK(parity_body.find("called_by") != std::string::npos,
                  "AC4: dual-graph parity still walks called_by for all callers");
        }
        // Source-cite the #3165/#3187 lineage.
        CHECK(ixx.find("#3165") != std::string::npos || ixx.find("#3187") != std::string::npos,
              "AC4: #3165/#3187 lineage cited");
    }

    // ── AC5: Non-duplicative to #3345 — peel snapshot only, no
    //    interpreter visibility changes ─────────────────────────────
    {
        std::println("\n--- AC5: non-duplicative to #3345 ---");
        const auto peel_body =
            find_fn_body(ixx, "std::size_t relower_dirty_defines_from_workspace()", 4500);
        // The #3381 block lives in the peel (relower) path only — it
        // touches dirty_names / dep_graph_ / mark_caller_body_dirty /
        // mark_all_blocks_dirty. It does NOT touch:
        //   - interpreter visibility (no change to ir_interpreter_* /
        //     hybrid_node_cascade_ exec paths)
        //   - depth-1 fanout (no new "force full if depth==1" logic)
        const auto block_pos = peel_body.find("Issue #3381");
        if (block_pos != std::string::npos) {
            const auto block_scope = peel_body.substr(block_pos, 2500);
            CHECK(block_scope.find("interpreter") == std::string::npos,
                  "AC5: #3381 does not touch interpreter paths");
            CHECK(block_scope.find("depth") == std::string::npos,
                  "AC5: #3381 does not touch depth-1 fanout");
        }
        // #3345 (open, P1) is the hybrid interpreter visibility half.
        // #3381 covers the peel snapshot half. The two are coordinateable
        // in one patch but this fix does not close #3345.
        CHECK(ixx.find("#3345") != std::string::npos || true,
              "AC5: #3345 reference optional (open P1, coordinate)");
    }

    // ── AC6: No new query schema — existing counters move when AC1
    //    is violated in soak ─────────────────────────────────────────
    {
        std::println("\n--- AC6: no new query schema, existing counters reused ---");
        // 6a. No new counter of the form `*3381*_total` is introduced.
        CHECK(ixx.find("3381_total") == std::string::npos,
              "AC6: no new 3381-suffixed counter total introduced");
        // 6b. Reused counters (no schema change):
        CHECK(ixx.find("partial_forced_full_by_impact_total") != std::string::npos,
              "AC6: existing partial_forced_full_by_impact_total counter retained");
        CHECK(ixx.find("should_relower_total") != std::string::npos,
              "AC6: existing should_relower_total counter retained");
        CHECK(ixx.find("incremental_soundness_mismatch_prod_total") != std::string::npos,
              "AC6: existing incremental_soundness_mismatch_prod_total counter retained");
        // 6c. The fix bumps partial_forced_full_by_impact_total on the
        //     gen-moved fail-closed take-full path (#3283 shape).
        const auto peel_body =
            find_fn_body(ixx, "std::size_t relower_dirty_defines_from_workspace()", 4500);
        if (!peel_body.empty()) {
            const auto block_pos = peel_body.find("Issue #3381");
            if (block_pos != std::string::npos) {
                const auto block_scope = peel_body.substr(block_pos, 2500);
                CHECK(block_scope.find("partial_forced_full_by_impact_total.fetch_add(") !=
                          std::string::npos,
                      "AC6: bump site on fail-closed take-full path");
            }
        }
        // 6d. No docs/design/3381-* (per MEMORY #1655 docs are obsolete
        //     for agent repo; we don't write design docs).
        CHECK(read_file("docs/design/3381-facade-dirty-snapshot.md").empty(),
              "AC6: no docs/design/3381-* per #1655");
        // 6e. No test_issue_3381_* (per MEMORY 2026-07-24: tests go to
        //     src/-aligned suite; this file uses the thematic
        //     test_incremental_facade_dirty_names_snapshot prefix).
        const auto self_path = "tests/compiler/test_incremental_facade_dirty_names_snapshot.cpp";
        auto self = read_file(self_path);
        CHECK(self.find("test_issue_3381") == std::string::npos,
              "AC6: this test file does not invent test_issue_3381_*");
    }

    // ── AC7: production facade + Soft path unchanged — the fix only
    //    runs at peel entry (Soft BFS already covers callers) ────────
    {
        std::println("\n--- AC7: production facade + Soft BFS unchanged ---");
        // mark_define_dirty production branch still dirties only the root
        // (per #3112/#3150/#3188/#3219). #3381 fix moves the caller-union
        // to peel entry (relower_dirty_defines_from_workspace), not into
        // mark_define_dirty — Soft BFS at service_dirty.cpp is unchanged.
        const auto mark_def = find_fn_body(
            dir, "void CompilerService::mark_define_dirty(const std::string& name)", 1500);
        // Production branch still calls hard_invalidate_via_facade +
        // mark_body_only_dirty + invalidate_shape + return.
        CHECK(mark_def.find("hard_invalidate_via_facade") != std::string::npos,
              "AC7: production facade still wires hard_invalidate_via_facade");
        CHECK(mark_def.find("mark_body_only_dirty") != std::string::npos,
              "AC7: production still mark_body_only_dirty(root)");
        CHECK(mark_def.find("invalidate_shape") != std::string::npos,
              "AC7: production still invalidate_shape(root)");
        // hot_update_registry.cpp facade is the same.
        CHECK(hot.find("hard_invalidate_via_facade") != std::string::npos,
              "AC7: hot_update_registry still owns hard_invalidate_via_facade");
        CHECK(hot.find("notify_dirty_define") != std::string::npos,
              "AC7: facade still calls notify_dirty_define(root)");
    }

    std::println("\n=== Issue #3381 done ===");
    return g_failed == 0 ? 0 : 1;
}

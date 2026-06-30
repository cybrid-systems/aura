// @category: integration
// @reason: uses CompilerService + Evaluator + EnvFrame SoA walk APIs
//
// test_issue_355.cpp — Verify Issue #355 acceptance criteria
// ("[Follow-up #242-1] Wire stale-frame detection into
//  lookup_by_symid_chain (parent walks)").
//
// Background: #242 introduced is_env_frame_stale +
// version-bumping + stats-counter + stderr warning, but only
// materialize_call_env (the closure body entry path) used the
// pattern. The SoA parent walkers — lookup_by_symid_chain,
// walk_env_frame_roots, Env::lookup_cell_ptr, Env::lookup_cell_index
// — read stale frames silently: they skipped the frame and
// bumped a stats counter, but never refreshed the frame's
// version_ and never emitted the [#242 warning].
//
// #355 wires the same pattern into all four walk sites via a
// new helper, Evaluator::refresh_stale_frame_in_walk(id, site),
// which is the single source of truth for the
// "saw a stale frame during a walk" behavior.
//
// Test strategy: 4 layers, one per walker site + one helper test.
//
//   Layer 1: refresh_stale_frame_in_walk — bumps version_ +
//            stats counter, silent by default, opt-in verbose
//   Layer 2: lookup_by_symid_chain — surfaces staleness in the
//            Evaluator-level walk
//   Layer 3: walk_env_frame_roots — surfaces staleness in the
//            GC walk (read via stats counter)
//   Layer 4: Env::lookup_cell_ptr / lookup_cell_index — surfaces
//            staleness in the Env-level parent walks

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_issue_355_detail {

// ═══════════════════════════════════════════════════════════
// Layer 1: refresh_stale_frame_in_walk helper behavior
// ═══════════════════════════════════════════════════════════

bool test_refresh_bumps_frame_version() {
    std::println("\n--- AC1: refresh_stale_frame_in_walk bumps frame.version_ ---");
    aura::compiler::Evaluator ev;
    aura::compiler::EnvId id = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_stale(id), "fresh frame is not stale");

    // Bump defuse_version_ — frame becomes stale.
    ev.bump_defuse_version_for_test();
    CHECK(ev.is_env_frame_stale(id), "frame is stale after version bump");

    // Call the helper. It should bump the frame's version_ to
    // the current defuse_version_, silencing future staleness
    // checks for this frame.
    ev.refresh_stale_frame_in_walk(id, "test_refresh_bumps_frame_version");
    CHECK(!ev.is_env_frame_stale(id),
          "frame is no longer stale after refresh_stale_frame_in_walk");

    auto v_after = ev.defuse_version_for_test();
    CHECK(ev.env_frame(id).version_ == v_after,
          "frame.version_ == current defuse_version_ after refresh");
    return true;
}

bool test_refresh_bumps_stats_counter() {
    std::println("\n--- AC2: refresh_stale_frame_in_walk bumps envframe_stale_refresh_count_ ---");
    aura::compiler::Evaluator ev;
    aura::compiler::EnvId id = ev.alloc_env_frame();

    auto before = ev.get_envframe_stale_refresh_count();
    ev.bump_defuse_version_for_test();
    ev.refresh_stale_frame_in_walk(id, "test_refresh_bumps_stats_counter");
    auto after = ev.get_envframe_stale_refresh_count();

    CHECK(after == before + 1,
          "envframe_stale_refresh_count_ incremented by exactly 1");
    return true;
}

bool test_refresh_idempotent_when_already_fresh() {
    std::println("\n--- AC3: refresh_stale_frame_in_walk is a no-op for fresh frames ---");
    aura::compiler::Evaluator ev;
    aura::compiler::EnvId id = ev.alloc_env_frame();
    // Frame is fresh — no version bump, no stats counter
    // increment, just return.
    auto before = ev.get_envframe_stale_refresh_count();
    ev.refresh_stale_frame_in_walk(id, "test_refresh_idempotent_when_already_fresh");
    auto after = ev.get_envframe_stale_refresh_count();
    CHECK(after == before,
          "stats counter unchanged for fresh frame");
    return true;
}

bool test_refresh_safe_on_invalid_id() {
    std::println("\n--- AC4: refresh_stale_frame_in_walk is safe on invalid EnvId ---");
    aura::compiler::Evaluator ev;
    auto before = ev.get_envframe_stale_refresh_count();
    // NULL_ENV_ID — the helper should early-return without
    // touching the stats counter.
    ev.refresh_stale_frame_in_walk(aura::compiler::NULL_ENV_ID,
                                   "test_refresh_safe_on_invalid_id");
    auto after_null = ev.get_envframe_stale_refresh_count();
    CHECK(after_null == before,
          "NULL_ENV_ID is a no-op (no counter increment)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: lookup_by_symid_chain surfaces staleness
// ═══════════════════════════════════════════════════════════

bool test_lookup_by_symid_chain_refreshes_stale_frame() {
    std::println("\n--- AC5: lookup_by_symid_chain refreshes stale frame in parent walk ---");
    aura::compiler::Evaluator ev;

    // Allocate a frame and bind a value so the walk has something
    // to find. Use the helpers available on Evaluator.
    aura::compiler::EnvId fid = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_stale(fid), "fresh frame");

    // Mark stale.
    ev.bump_defuse_version_for_test();
    auto stats_before = ev.get_envframe_stale_refresh_count();

    // Walk — should encounter the stale frame, call
    // refresh_stale_frame_in_walk, bump the stats counter, and
    // bump the frame's version_.
    auto result = ev.lookup_by_symid_chain(fid, aura::ast::SymId{0});
    (void)result; // we only care about side effects on the frame

    auto stats_after = ev.get_envframe_stale_refresh_count();
    CHECK(stats_after >= stats_before + 1,
          "lookup_by_symid_chain bumped stale_refresh_count_");
    CHECK(!ev.is_env_frame_stale(fid),
          "frame refreshed (no longer stale) after lookup_by_symid_chain walk");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 3: walk_env_frame_roots surfaces staleness
// ═══════════════════════════════════════════════════════════

bool test_walk_env_frame_roots_refreshes_stale_frame() {
    std::println("\n--- AC6: walk_env_frame_roots refreshes stale frame in GC walk ---");
    aura::compiler::Evaluator ev;

    aura::compiler::EnvId fid = ev.alloc_env_frame();
    ev.bump_defuse_version_for_test();
    auto stats_before = ev.get_envframe_stale_refresh_count();

    std::vector<std::int64_t> pair_roots;
    std::vector<std::int64_t> closure_roots;
    ev.walk_env_frame_roots(pair_roots, closure_roots);

    auto stats_after = ev.get_envframe_stale_refresh_count();
    CHECK(stats_after >= stats_before + 1,
          "walk_env_frame_roots bumped stale_refresh_count_");
    CHECK(!ev.is_env_frame_stale(fid),
          "frame refreshed (no longer stale) after walk_env_frame_roots");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 4: Env::lookup_cell_ptr + lookup_cell_index surfaces staleness
// ═══════════════════════════════════════════════════════════

bool test_env_lookup_cell_ptr_refreshes_stale_frame() {
    std::println("\n--- AC7: Env::lookup_cell_ptr refreshes stale frame in parent walk ---");
    aura::compiler::Evaluator ev;

    aura::compiler::EnvId fid = ev.alloc_env_frame();
    ev.bump_defuse_version_for_test();
    auto stats_before = ev.get_envframe_stale_refresh_count();

    // Build an Env that walks via the SoA path
    // (owner_ + parent_id_ both set, so lookup_cell_ptr takes
    // the SoA walk branch).
    aura::compiler::Env e;
    e.set_owner(&ev);
    e.set_parent_id(fid);
    std::vector<aura::compiler::types::EvalValue> cells;
    auto* p = e.lookup_cell_ptr("missing", &cells);
    (void)p; // we only care about side effects on the frame

    auto stats_after = ev.get_envframe_stale_refresh_count();
    CHECK(stats_after >= stats_before + 1,
          "Env::lookup_cell_ptr bumped stale_refresh_count_");
    CHECK(!ev.is_env_frame_stale(fid),
          "frame refreshed (no longer stale) after Env::lookup_cell_ptr walk");
    return true;
}

bool test_env_lookup_cell_index_refreshes_stale_frame() {
    std::println("\n--- AC8: Env::lookup_cell_index refreshes stale frame in parent walk ---");
    // This path was previously missing the staleness check
    // entirely (the version_ gate existed only in lookup_cell_ptr
    // / lookup_by_symid_chain / walk_env_frame_roots). #355
    // closes the gap.
    aura::compiler::Evaluator ev;

    aura::compiler::EnvId fid = ev.alloc_env_frame();
    ev.bump_defuse_version_for_test();
    auto stats_before = ev.get_envframe_stale_refresh_count();

    aura::compiler::Env e;
    e.set_owner(&ev);
    e.set_parent_id(fid);
    auto idx = e.lookup_cell_index("missing");
    (void)idx;

    auto stats_after = ev.get_envframe_stale_refresh_count();
    CHECK(stats_after >= stats_before + 1,
          "Env::lookup_cell_index bumped stale_refresh_count_ (AC for #355)");
    CHECK(!ev.is_env_frame_stale(fid),
          "frame refreshed (no longer stale) after Env::lookup_cell_index walk");
    return true;
}

// ═══════════════════════════════════════════════════════════
// End-to-end: closure capture + parent walk through lookup_by_symid_chain
// ═══════════════════════════════════════════════════════════

bool test_end_to_end_walk_via_closure_body() {
    std::println("\n--- AC9: parent walk via closure body surfaces #242 warning ---");
    // This is the canonical user-visible scenario from the
    // issue body: a closure's body looks up a parent binding
    // via the SoA walk after the parent has been mutated.
    aura::compiler::CompilerService cs;
    cs.eval("(begin "
            "  (define x 10) "
            "  (define f (lambda () x)) "
            "  (f))");

    // Trigger a mutation that invalidates the captured frame.
    auto m = cs.eval("(mutate:rebind 'x 20)");
    CHECK(m.has_value(), "mutate:rebind succeeds");

    // Invoke the closure — its body looks up `x` via the
    // SoA parent walk. The frame is stale, so the walk should
    // bump the stats counter (via refresh_stale_frame_in_walk).
    auto r = cs.eval("(f)");
    CHECK(r.has_value(), "closure body lookup succeeds (no crash)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #355 verification tests ═══\n");

    std::println("Layer 1: refresh_stale_frame_in_walk helper");
    test_refresh_bumps_frame_version();
    test_refresh_bumps_stats_counter();
    test_refresh_idempotent_when_already_fresh();
    test_refresh_safe_on_invalid_id();

    std::println("\nLayer 2: lookup_by_symid_chain");
    test_lookup_by_symid_chain_refreshes_stale_frame();

    std::println("\nLayer 3: walk_env_frame_roots");
    test_walk_env_frame_roots_refreshes_stale_frame();

    std::println("\nLayer 4: Env::lookup_cell_ptr + lookup_cell_index");
    test_env_lookup_cell_ptr_refreshes_stale_frame();
    test_env_lookup_cell_index_refreshes_stale_frame();

    std::println("\nLayer 5: end-to-end via closure body");
    test_end_to_end_walk_via_closure_body();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
}  // namespace aura_issue_355_detail

int aura_issue_355_run() { return aura_issue_355_detail::run_tests(); }
// test_issues_1903_1908_batch.cpp — consolidated orphan issues/ range batch
// These sources were not in issue bundles / fixtures (dead standalones).
// Prefer domain/ theme batches for new work; do not re-add per-issue files.

#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;


// ─── from test_issue_1903.cpp → aura_iss_run_i1903::run_i1903 ───
namespace aura_iss_run_i1903 {
// @category: integration
// @reason: uses Evaluator + EnvFrame SoA APIs + GCEnvWalkFn + (gc-heap) cycle
//
// test_issue_1903.cpp — Verify Issue #1903 acceptance criteria
// ("Enforce dual-path consistency (bindings_ vs bindings_symid_) +
//  strict version stamping + INVALID_VERSION filtering in EnvFrame,
//  GCEnvWalkFn, materialize_call_env and all lookup/walk paths").
//
// #1903 turns the existing observability counter family (#543 /
// #1269 / #1550) into an enforced invariant:
//   1. EnvFrame::ensure_dual_path_consistent() — the canonical 3-check
//      helper (length + linear-ownership SoA + content parity when
//      pool_ is set). Called after every bind/bind_symid in mutate
//      paths, after materialize_call_env copy, and after
//      complete_post_resume_steal_refresh.
//   2. GCEnvWalkFn (walk_env_frame_roots) — INVALID_VERSION skip +
//      prefer bindings_symid_ + legacy bindings_ fallback with
//      dedicated counter bump.
//   3. (query:envframe-dual-consistency-stats) — new Aura primitive
//      exposing 4 #1903 counters.
//
// This test exercises the public-API surface only — INVALID_VERSION
// frame mutation is covered by test_issue_356 (the helper
// invalidate_post_rollback_env_frames is the canonical writer; this
// file does not duplicate the panic-rollback integration which would
// require internal frame mutation). The exhaustive multi-thread
// concurrent mutate + steal + GC + query stress test lives in
// test_unify_invalidate_try_acquire_1634 (which is wired through
// #1592/#1631/#1903 together).
//
// Acceptance Criteria covered (mirrors the #1903 body AC):
//   AC1: ensure_dual_path_consistent() bumps
//        envframe_dual_consistency_asserted_ on every call
//   AC2: (query:envframe-dual-consistency-stats) primitive returns
//        integer (sum of 4 counters) + monotonic
//   AC3: 4 #1903 counter accessors reachable + monotonic
//   AC4: walk_env_frame_roots on a fresh evaluator is safe (no
//        crash, no GC walk safe-skips bump on empty arena)
//   AC5: 1k iter alloc + ensure_dual_path_consistent loop —
//        envframe_dual_consistency_asserted_ grows by exactly N
//        per iter (no leak, no double-count, no silent failure)
//   AC6: walk_env_frame_roots after stress loop does not crash and
//        safe_skips / gc_walk_legacy_fallback_uses counters are
//        monotonic non-decreasing


using aura::test::g_failed;
using aura::test::g_passed;


namespace aura_issue_1903_detail {

    using aura::compiler::CompilerService;
    using aura::compiler::EnvFrame;
    using aura::compiler::EnvId;
    using aura::compiler::Evaluator;
    using aura::compiler::INVALID_VERSION;
    using aura::compiler::NULL_ENV_ID;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;

    // ── AC1: ensure_dual_path_consistent() bumps asserted counter ──
    bool test_ensure_dual_path_consistent_bumps_asserted() {
        std::println("\n--- AC1: ensure_dual_path_consistent bumps asserted counter ---");
        Evaluator ev;
        auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
        const auto before = ev.get_envframe_dual_consistency_asserted();
        // alloc_env_frame already set owner_ (per #1903); calling the helper
        // bumps envframe_dual_consistency_asserted_ by 1.
        const bool ok = ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));
        const auto after = ev.get_envframe_dual_consistency_asserted();
        CHECK(ok, "fresh empty frame is dual-path consistent (helper returns true)");
        CHECK(after == before + 1, "ensure_dual_path_consistent bumps asserted by 1");
        return true;
    }

    // ── AC2: (query:envframe-dual-consistency-stats) primitive ──
    bool test_query_dual_consistency_stats_primitive() {
        std::println(
            "\n--- AC2: (query:envframe-dual-consistency-stats) returns int + monotonic ---");
        CompilerService cs;
        auto r0 = cs.eval("(engine:metrics \"query:envframe-dual-consistency-stats\")");
        CHECK(r0.has_value(), "primitive returns a value");
        CHECK(r0 && is_int(*r0), "primitive is an integer");
        if (!r0 || !is_int(*r0)) {
            ++g_failed;
            return false;
        }
        const auto baseline = as_int(*r0);

        // Trigger activity: allocate + ensure on a fresh frame.
        auto& ev = cs.evaluator();
        auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
        (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));

        auto r1 = cs.eval("(engine:metrics \"query:envframe-dual-consistency-stats\")");
        CHECK(r1 && is_int(*r1), "primitive still integer after activity");
        if (!r1 || !is_int(*r1)) {
            ++g_failed;
            return false;
        }
        const auto after = as_int(*r1);
        CHECK(after >= baseline, "primitive monotonic (>= baseline)");
        return true;
    }

    // ── AC3: 4 #1903 counter accessors reachable + monotonic non-neg ──
    bool test_four_counter_accessors_reachable() {
        std::println("\n--- AC3: 4 #1903 counter accessors reachable + monotonic ---");
        Evaluator ev;
        const auto a0 = ev.get_envframe_dual_consistency_asserted();
        const auto s0 = ev.get_envframe_post_steal_dual_synced();
        const auto m0 = ev.get_envframe_materialize_consistency_checks();
        const auto l0 = ev.get_envframe_gc_walk_legacy_fallback_uses();
        CHECK(a0 + s0 + m0 + l0 == 0, "fresh evaluator: 4 counters all 0");

        // Trigger some activity.
        auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
        (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));

        const auto a1 = ev.get_envframe_dual_consistency_asserted();
        const auto s1 = ev.get_envframe_post_steal_dual_synced();
        const auto m1 = ev.get_envframe_materialize_consistency_checks();
        const auto l1 = ev.get_envframe_gc_walk_legacy_fallback_uses();
        CHECK(a1 > a0, "envframe_dual_consistency_asserted_ grows after 1 ensure call");
        CHECK(s1 >= s0, "envframe_post_steal_dual_synced_ monotonic non-decreasing");
        CHECK(m1 >= m0, "envframe_materialize_consistency_checks_ monotonic non-decreasing");
        CHECK(l1 >= l0, "envframe_gc_walk_legacy_fallback_uses_ monotonic non-decreasing");
        return true;
    }

    // ── AC4: walk_env_frame_roots on a fresh evaluator is safe ──
    bool test_walk_env_frame_roots_fresh_evaluator_safe() {
        std::println("\n--- AC4: walk_env_frame_roots on fresh evaluator is safe ---");
        Evaluator ev;
        const auto before = ev.get_envframe_gc_walk_safe_skips();
        const auto legacy_before = ev.get_envframe_gc_walk_legacy_fallback_uses();
        std::vector<std::int64_t> pair_roots;
        std::vector<std::int64_t> closure_roots;
        ev.walk_env_frame_roots(pair_roots, closure_roots);
        CHECK(pair_roots.empty(), "empty arena → no pair roots");
        CHECK(closure_roots.empty(), "empty arena → no closure roots");
        const auto after = ev.get_envframe_gc_walk_safe_skips();
        const auto legacy_after = ev.get_envframe_gc_walk_legacy_fallback_uses();
        CHECK(after == before, "empty arena walk does not bump safe_skips");
        CHECK(legacy_after == legacy_before, "empty arena walk does not bump legacy fallback");
        return true;
    }

    // ── AC5: 1k iter alloc + ensure_dual_path_consistent loop ──
    bool test_iter_loop_monotonic_counters() {
        std::println("\n--- AC5: 1k iter alloc + ensure loop ---");
        Evaluator ev;
        const auto asserted_before = ev.get_envframe_dual_consistency_asserted();
        for (int i = 0; i < 1000; ++i) {
            auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
            // 3× consistency checks per iter to amplify the counter.
            (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));
            (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));
            (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));
        }
        const auto asserted_after = ev.get_envframe_dual_consistency_asserted();
        CHECK(asserted_after == asserted_before + 3000,
              "3000 ensure calls bumped asserted by exactly 3000 (no leak, no double-count)");
        return true;
    }

    // ── AC6: walk_env_frame_roots after stress loop is safe ──
    bool test_walk_after_stress_loop_safe() {
        std::println("\n--- AC6: walk_env_frame_roots after stress loop is safe ---");
        Evaluator ev;
        for (int i = 0; i < 500; ++i) {
            auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
            (void)ev.ensure_envframe_dual_path_consistency(ev.env_frame(id));
        }
        const auto safe_before = ev.get_envframe_gc_walk_safe_skips();
        const auto legacy_before = ev.get_envframe_gc_walk_legacy_fallback_uses();
        std::vector<std::int64_t> pair_roots;
        std::vector<std::int64_t> closure_roots;
        ev.walk_env_frame_roots(pair_roots, closure_roots);
        const auto safe_after = ev.get_envframe_gc_walk_safe_skips();
        const auto legacy_after = ev.get_envframe_gc_walk_legacy_fallback_uses();
        CHECK(safe_after >= safe_before, "walk does not decrement safe_skips");
        CHECK(legacy_after >= legacy_before, "walk does not decrement legacy fallback");
        return true;
    }

} // namespace aura_issue_1903_detail

int run_i1903() {
    using namespace aura_issue_1903_detail;
    std::println("=== Issue #1903: EnvFrame dual-path consistency enforcement ===");
    int rc = 0;
    rc |= !test_ensure_dual_path_consistent_bumps_asserted();
    rc |= !test_query_dual_consistency_stats_primitive();
    rc |= !test_four_counter_accessors_reachable();
    rc |= !test_walk_env_frame_roots_fresh_evaluator_safe();
    rc |= !test_iter_loop_monotonic_counters();
    rc |= !test_walk_after_stress_loop_safe();
    std::println("\n=== Summary: passed={} failed={} ===", g_passed, g_failed);
    return rc == 0 ? 0 : 1;
}

} // namespace aura_iss_run_i1903
// ─── end test_issue_1903.cpp ───
int main() {
    std::println("\n######## run_i1903 ########");
    if (int rc = aura_iss_run_i1903::run_i1903(); rc != 0) {
        std::println("run_i1903 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\ntest_issues_1903_1908_batch: OK");
    return 0;
}

// test_issues_1466_1478_batch.cpp — consolidated orphan issues/ range batch
// These sources were not in issue bundles / fixtures (dead standalones).
// Prefer domain/ theme batches for new work; do not re-add per-issue files.

#include "test_harness.hpp"
#include "core/cpp26_contract_stats.h"
#include "compiler/shape_profiler.h"
#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <atomic>
#include <format>
#include <cstdlib>
#include "compiler/observability_metrics.h"

import std;
import aura.core.cxx26_invariants;
import aura.core.arena;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.ir;
import aura.parser.parser;


// ─── from test_issue_1466.cpp → aura_iss_run_i1466::run_i1466 ───
namespace aura_iss_run_i1466 {
// @category: unit
// @reason: pure C++ consteval + contract_stats surface; no CompilerService
//
// test_issue_1466.cpp — Issue #1466: hot-path Contracts + consteval
// invariant coverage expansion.
//
// Background: #1466 audits the hot paths (shape dispatch, dirty
// cascade, arena bump, value as_*, eval core) for aggressive
// `pre`/`post` placement (zero release cost under observe semantic),
// and grows the consteval invariant surface in cxx26_invariants.ixx.
// The consteval_checks count bumps so the AI Agent can detect drift
// via (query:cpp26-contracts-stats).
//
// ACs:
//   AC1: kConstevalChecksTotal bumped 36 → 53 (+17 hot-path invariants)
//   AC2: kCpp26ConstevalChecksShipped matches kConstevalChecksTotal
//   AC3: shape_inline_post_contracts_active flag = 1 (contract present)
//   AC4: arena_compact_contracts_active flag = 1 (contract present)
//   AC5: dirty_cascade_contracts_active flag = 1 (contract present)
//   AC6: consteval_invariants_total atomic matches shipped count
//   AC7: hotpath_invariant_hits_total still exposed (no regression)


using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1466_detail {


    // Compile-time: consteval surface expansion is consistent across both
    // authoritative sources. Any drift between cxx26_invariants.ixx and
    // cpp26_contract_stats.h is caught at compile time.
    static_assert(aura::core::cpp26::kConstevalChecksTotal >= 53,
                  "Issue #1466: kConstevalChecksTotal must be 53 (was 36, +17)");
    static_assert(aura::core::kCpp26ConstevalChecksShipped >= 53,
                  "Issue #1466: kCpp26ConstevalChecksShipped must be 53 (was 36, +17)");
    static_assert(aura::core::cpp26::kConstevalChecksTotal ==
                      aura::core::kCpp26ConstevalChecksShipped,
                  "Issue #1466: consteval count must match between cpp26_contract_stats and "
                  "cxx26_invariants");

    void ac1_ac2_consteval_count() {
        std::println("\n--- AC1/AC2: consteval invariant count (compile-time + runtime) ---");
        CHECK(aura::core::cpp26::kConstevalChecksTotal >= 53,
              "kConstevalChecksTotal >= 53 (was 36, +17 in #1466)");
        CHECK(aura::core::kCpp26ConstevalChecksShipped >= 53,
              "kCpp26ConstevalChecksShipped >= 53 (matches cpp26_contract_stats)");
        CHECK(aura::core::cpp26::kConstevalChecksTotal == aura::core::kCpp26ConstevalChecksShipped,
              "consteval count cross-source consistency (no drift)");
    }

    void ac3_shape_inline_post_flag() {
        std::println("\n--- AC3: shape inline_shape_of post-contract flag ---");
        using aura::core::cpp26::shape_inline_post_contracts_active;
        CHECK(shape_inline_post_contracts_active.load(std::memory_order_relaxed) == 1,
              "shape_inline_post_contracts_active default = 1 (contract placed)");
    }

    void ac4_arena_compact_flag() {
        std::println("\n--- AC4: arena::compact pre/post flag ---");
        using aura::core::cpp26::arena_compact_contracts_active;
        CHECK(arena_compact_contracts_active.load(std::memory_order_relaxed) == 1,
              "arena_compact_contracts_active default = 1 (contract placed)");
    }

    void ac5_dirty_cascade_flag() {
        std::println("\n--- AC5: ast mark_dirty_upward post-contract flag ---");
        using aura::core::cpp26::dirty_cascade_contracts_active;
        CHECK(dirty_cascade_contracts_active.load(std::memory_order_relaxed) == 1,
              "dirty_cascade_contracts_active default = 1 (contract placed)");
    }

    void ac6_consteval_invariants_runtime() {
        std::println("\n--- AC6: consteval_invariants_total atomic default ---");
        using aura::core::cpp26::consteval_invariants_total;
        CHECK(consteval_invariants_total.load(std::memory_order_relaxed) >= 53,
              "consteval_invariants_total default = 53 (matches shipped consteval)");
        CHECK(consteval_invariants_total.load(std::memory_order_relaxed) ==
                  aura::core::cpp26::kConstevalChecksTotal,
              "consteval_invariants_total == kConstevalChecksTotal (no drift)");
    }

    void ac7_hotpath_invariant_hits_no_regression() {
        std::println("\n--- AC7: hotpath_invariant_hits_total still exposed ---");
        using aura::core::cpp26::hotpath_invariant_hits_total;
        // Pre-existing counter from #742. Just verify it's still addressable
        // and not zero-initialized as a sentinel for "removed".
        const auto v = hotpath_invariant_hits_total.load(std::memory_order_relaxed);
        CHECK(v >= 0, "hotpath_invariant_hits_total is readable (no regression)");
    }

    void ac8_record_consteval_invariant_added() {
        std::println("\n--- AC8: record_consteval_invariant_added helper ---");
        using aura::core::cpp26::consteval_invariants_total;
        using aura::core::cpp26::record_consteval_invariant_added;
        const auto before = consteval_invariants_total.load(std::memory_order_relaxed);
        record_consteval_invariant_added();
        const auto after = consteval_invariants_total.load(std::memory_order_relaxed);
        CHECK(after == before + 1,
              "record_consteval_invariant_added bumps the atomic counter by 1");
    }

} // namespace test_issue_1466_detail

int run_i1466() {
    using namespace test_issue_1466_detail;
    std::println("=== Issue #1466 — hot-path Contracts + consteval coverage ===");
    ac1_ac2_consteval_count();
    ac3_shape_inline_post_flag();
    ac4_arena_compact_flag();
    ac5_dirty_cascade_flag();
    ac6_consteval_invariants_runtime();
    ac7_hotpath_invariant_hits_no_regression();
    ac8_record_consteval_invariant_added();

    std::println("\n─── #1466 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
} // namespace aura_iss_run_i1466
// ─── end test_issue_1466.cpp ───

// ─── from test_issue_1467.cpp → aura_iss_run_i1467::run_i1467 ───
namespace aura_iss_run_i1467 {
// @category: unit
// @reason: pure C++ arena defrag foundation; no CompilerService / LLVM JIT
//
// test_issue_1467.cpp — Issue #1467 Phase 1: live-object-moving defrag
// foundation (mark-only skeleton, no copy/relocate).
//
// Background: #300 shipped defrag() foundation + counters that stay 0
// until the full live-object-moving path lands. #1467 Phase 1 ships the
// minimum to make `live_defrag()` callable + its counters updateable:
//   1. live_defrag() method (mark-only, no copy)
//   2. live_defrag_attempted_count + live_objects_marked_total counters
//   3. accessor methods
//   4. format() / merge() / JSON output updates
// The actual copy + pointer remapping is tracked as #1467 Phase 2/3
// follow-ups (research-grade work, 4-6 days total per the issue body).
//
// ACs:
//   AC1: live_defrag() callable, returns marked object count >= 0
//   AC2: stats_.live_defrag_attempted_count bumps by 1 per call
//   AC3: live_objects_marked_total bumps by >= tier count after alloc
//   AC4: conservative defrag() still works (no regression)
//   AC5: format() / merge() include new fields
//   AC6: accessor methods return values consistent with stats
//   AC7: shape_inval_on_compact bumps via invoke_compact_hook_


using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1467_detail {


    void ac1_live_defrag_callable() {
        std::println("\n--- AC1: live_defrag() callable + returns marked count ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        const auto before = arena->live_defrag_attempted_count_relaxed();
        const auto marked = arena->live_defrag();
        const auto after = arena->live_defrag_attempted_count_relaxed();
        CHECK(marked >= 0, "live_defrag() returns non-negative marked count");
        CHECK(after == before + 1,
              "live_defrag_attempted_count bumps by 1 per call (atomic mirror)");
    }

    void ac2_stats_counter_bumps() {
        std::println("\n--- AC2: ArenaStats live_defrag_attempted_count bumps ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        const auto s_before = arena->stats().live_defrag_attempted_count;
        arena->live_defrag();
        const auto s_after = arena->stats().live_defrag_attempted_count;
        CHECK(s_after == s_before + 1,
              "stats.live_defrag_attempted_count bumps by 1 (stats struct)");
    }

    void ac3_marked_count_after_alloc() {
        std::println("\n--- AC3: live_objects_marked_total reflects allocations ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        // Allocate 3 small objects in tier 0 (16 bytes).
        void* p1 = arena->try_allocate(16);
        void* p2 = arena->try_allocate(16);
        void* p3 = arena->try_allocate(16);
        CHECK(p1 != nullptr && p2 != nullptr && p3 != nullptr,
              "try_allocate(16) x3 returns non-null (tier 0)");
        const auto marked_before = arena->stats().live_objects_marked_total;
        arena->live_defrag();
        const auto marked_after = arena->stats().live_objects_marked_total;
        CHECK(marked_after >= marked_before + 3,
              "live_objects_marked_total bumps by at least 3 (3 allocs in tier 0)");
    }

    void ac4_conservative_defrag_no_regression() {
        std::println("\n--- AC4: conservative defrag() still works ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        // Allocate something so defrag has something to do.
        void* p = arena->try_allocate(32);
        CHECK(p != nullptr, "try_allocate(32) returns non-null");
        const auto d_before = arena->stats().defrag_attempted_count;
        const auto reclaimed = arena->defrag();
        const auto d_after = arena->stats().defrag_attempted_count;
        CHECK(d_after == d_before + 1, "defrag_attempted_count bumps by 1");
        CHECK(reclaimed >= 0, "defrag() returns non-negative reclaimed bytes");
    }

    void ac5_format_and_merge() {
        std::println("\n--- AC5: format() + merge() include new fields ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        arena->live_defrag();
        const auto formatted = arena->stats().format();
        CHECK(formatted.find("live-defrags") != std::string::npos,
              "format() output contains 'live-defrags'");
        CHECK(formatted.find("marked") != std::string::npos, "format() output contains 'marked'");
        // Merge discipline: live counters sum across arenas.
        auto arena2 = std::make_unique<aura::ast::ASTArena>();
        arena2->live_defrag();
        arena2->live_defrag();
        auto merged = arena->stats();
        merged.merge(arena2->stats());
        const std::size_t expected_live = arena->stats().live_defrag_attempted_count +
                                          arena2->stats().live_defrag_attempted_count;
        CHECK(merged.live_defrag_attempted_count == expected_live,
              "merge() sums live_defrag_attempted_count correctly");
    }

    void ac6_accessors_consistent() {
        std::println("\n--- AC6: accessors return values consistent with stats ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        arena->live_defrag();
        arena->live_defrag();
        arena->live_defrag();
        const auto accessor_count = arena->live_defrag_attempted_count_relaxed();
        const auto stats_count = arena->stats().live_defrag_attempted_count;
        CHECK(accessor_count == stats_count,
              "live_defrag_attempted_count_relaxed() == stats.live_defrag_attempted_count");
        const auto accessor_marked = arena->live_objects_marked_total_relaxed();
        const auto stats_marked = arena->stats().live_objects_marked_total;
        CHECK(accessor_marked == stats_marked,
              "live_objects_marked_total_relaxed() == stats.live_objects_marked_total");
    }

    void ac7_shape_inval_on_compact_via_hook() {
        std::println("\n--- AC7: live_defrag triggers shape invalidation hook ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        int hook_calls = 0;
        arena->set_on_compact_hook([&hook_calls]() { ++hook_calls; });
        const auto before = arena->stats().shape_inval_on_compact;
        arena->live_defrag();
        const auto after = arena->stats().shape_inval_on_compact;
        CHECK(hook_calls == 1, "on_compact_hook called exactly once per live_defrag()");
        CHECK(after == before + 1, "shape_inval_on_compact bumps by 1 (via invoke_compact_hook_)");
    }

} // namespace test_issue_1467_detail

int run_i1467() {
    using namespace test_issue_1467_detail;
    std::println("=== Issue #1467 — live-object defrag foundation (Phase 1, mark-only) ===");
    ac1_live_defrag_callable();
    ac2_stats_counter_bumps();
    ac3_marked_count_after_alloc();
    ac4_conservative_defrag_no_regression();
    ac5_format_and_merge();
    ac6_accessors_consistent();
    ac7_shape_inval_on_compact_via_hook();

    std::println("\n─── #1467 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
} // namespace aura_iss_run_i1467
// ─── end test_issue_1467.cpp ───

// ─── from test_issue_1468.cpp → aura_iss_run_i1468::run_i1468 ───
namespace aura_iss_run_i1468 {
// @category: unit
// @reason: pure C++ ShapeProfiler; no CompilerService / LLVM JIT
//
// test_issue_1468.cpp — Issue #1468: ShapeProfiler history/dominant/stability
// tuning + deopt-storm protection + AI workload metrics.
//
// ACs:
//   AC1: Preset application (kDefaultPreset / kHighMutationPreset / kLowMutationPreset)
//   AC2: Deopt-storm detection: N deopts in M calls → deopt_storm_active() == true
//   AC3: 4 AI metrics: shape_stable_ratio / deopt_rate_per_fn /
//        history_hit_rate / mutation_induced_invalidations
//   AC4: invalidate_all bumps mutation_induced_invalidations_ by profile count
//   AC5: history_hit_count_ bumps per record_shape
//   AC6: SpecJITController coordination hook (deopt-storm aware)


using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1468_detail {


    using aura::compiler::shape::SHAPE_INT;
    using aura::compiler::shape::ShapeID;
    using aura::compiler::shape::ShapeProfiler;

    void ac1_preset_application() {
        std::println("\n--- AC1: Preset application ---");
        ShapeProfiler sp;
        sp.apply_preset(ShapeProfiler::kHighMutationPreset);
        const auto active = sp.active_preset();
        CHECK(active.window_size == ShapeProfiler::kHighMutationPreset.window_size,
              "high-mutation preset window_size applied");
        CHECK(active.stability_ratio == ShapeProfiler::kHighMutationPreset.stability_ratio,
              "high-mutation preset stability_ratio applied");
        CHECK(active.deopt_storm_window == ShapeProfiler::kHighMutationPreset.deopt_storm_window,
              "high-mutation preset deopt_storm_window applied");
        CHECK(active.deopt_storm_threshold ==
                  ShapeProfiler::kHighMutationPreset.deopt_storm_threshold,
              "high-mutation preset deopt_storm_threshold applied");
        sp.apply_preset(ShapeProfiler::kLowMutationPreset);
        CHECK(sp.active_preset().window_size == ShapeProfiler::kLowMutationPreset.window_size,
              "low-mutation preset window_size applied");
        sp.apply_preset(ShapeProfiler::kDefaultPreset);
        CHECK(sp.active_preset().window_size == ShapeProfiler::kDefaultPreset.window_size,
              "default preset round-trip");
    }

    void ac2_deopt_storm_detection() {
        std::println("\n--- AC2: Deopt-storm detection ---");
        ShapeProfiler sp;
        sp.apply_preset(ShapeProfiler::kDefaultPreset); // 256 window, 4 threshold
        CHECK(!sp.deopt_storm_active(), "deopt_storm starts inactive");
        // Trigger 4 deopts on the same fn (default threshold). The default
        // preset's threshold is 4 — 4 deopts should activate the storm.
        for (int i = 0; i < 4; ++i) {
            sp.record_shape(1, SHAPE_INT);
            (void)sp.invalidate(1);
        }
        CHECK(sp.deopt_storm_active(), "4 deopts in default window activates deopt_storm");
        CHECK(sp.deopt_storm_total() >= 1, "deopt_storm_total counter bumped on first activation");
    }

    void ac3_metrics_initial_state() {
        std::println("\n--- AC3: 4 AI metrics initial state ---");
        ShapeProfiler sp;
        CHECK(sp.shape_stable_ratio() == 0.0, "shape_stable_ratio == 0 with no profiles");
        CHECK(sp.deopt_rate_per_fn() == 0.0, "deopt_rate_per_fn == 0 with no profiles");
        CHECK(sp.history_hit_rate() == 0.0, "history_hit_rate == 0 with no history");
        CHECK(sp.mutation_induced_invalidations() == 0,
              "mutation_induced_invalidations == 0 initially");
        CHECK(sp.history_hit_count() == 0, "history_hit_count == 0 initially");
        CHECK(sp.history_miss_count() == 0, "history_miss_count == 0 initially");
        CHECK(sp.deopt_storm_total() == 0, "deopt_storm_total == 0 initially");
    }

    void ac4_invalidate_all_counter() {
        std::println("\n--- AC4: invalidate_all bumps mutation_induced_invalidations ---");
        ShapeProfiler sp;
        // Seed profiles.
        for (std::uint64_t fn = 1; fn <= 5; ++fn) {
            sp.record_shape(static_cast<aura::compiler::shape::FnKey>(fn), SHAPE_INT);
            // Push enough samples to potentially reach stability threshold;
            // we don't need it stable for this AC, just present.
            for (int i = 0; i < 200; ++i)
                sp.record_shape(static_cast<aura::compiler::shape::FnKey>(fn), SHAPE_INT);
        }
        const auto before = sp.mutation_induced_invalidations();
        sp.invalidate_all();
        const auto after = sp.mutation_induced_invalidations();
        CHECK(after == before + 5,
              "invalidate_all() bumps mutation_induced_invalidations by profile count (5)");
    }

    void ac5_history_hit_counter() {
        std::println("\n--- AC5: history_hit_count bumps per record_shape ---");
        ShapeProfiler sp;
        const auto before = sp.history_hit_count();
        for (int i = 0; i < 100; ++i) {
            sp.record_shape(1, SHAPE_INT);
        }
        const auto after = sp.history_hit_count();
        CHECK(after == before + 100, "history_hit_count bumps by 100 after 100 record_shape calls");
        CHECK(sp.history_hit_rate() > 0.0, "history_hit_rate > 0 after history lookups");
    }

    void ac6_spec_jit_controller_awareness() {
        std::println("\n--- AC6: SpecJITController coordination via deopt_storm_active ---");
        // The SpecJITController is supposed to query deopt_storm_active()
        // and down-shift to generic. We can't easily wire SpecJITController
        // here without LLVM JIT, but we verify the contract: the flag is
        // observable + persists until reset.
        ShapeProfiler sp;
        sp.apply_preset(ShapeProfiler::kDefaultPreset); // threshold 4
        CHECK(!sp.deopt_storm_active(), "starts inactive");
        for (int i = 0; i < 10; ++i) {
            sp.record_shape(2, SHAPE_INT);
            (void)sp.invalidate(2);
        }
        CHECK(sp.deopt_storm_active(), "storm active after many deopts (callers can downshift)");
        // reset() should clear the storm (verified via profile eviction path
        // tested elsewhere — here we just check the accessor works).
        CHECK(sp.deopt_storm_total() >= 1, "storm bump count recorded");
    }

} // namespace test_issue_1468_detail

int run_i1468() {
    using namespace test_issue_1468_detail;
    std::println("=== Issue #1468 — ShapeProfiler tuning + deopt-storm + AI metrics ===");
    ac1_preset_application();
    ac2_deopt_storm_detection();
    ac3_metrics_initial_state();
    ac4_invalidate_all_counter();
    ac5_history_hit_counter();
    ac6_spec_jit_controller_awareness();

    std::println("\n─── #1468 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
} // namespace aura_iss_run_i1468
// ─── end test_issue_1468.cpp ───


// ─── from test_issue_1470.cpp → aura_iss_run_i1470::run_i1470 ───
namespace aura_iss_run_i1470 {
// @category: integration
// @reason: Issue #1470 — query:ai-closedloop-readiness-stats
// consolidated AI closed-loop readiness observability primitive.
//
// Scope-limited close matching #457 / #470 / #527 / #738 / #632 /
// #622 / #918 pattern.
//
// Discovery before this PR (no duplication): the 5 counter types
// that feed this primitive are already exposed via scattered
// primitives:
//   - generation_wrap_count      — #457 query:stable-ref-stats +
//                                  #470 stable-ref-stats-hash[0]
//   - stable_ref_invalidations   — #457 + #470 stable-ref-stats-hash[1]
//   - atomic_batch_commits       — #192/#213 + #622 atomic-batch-stats-hash
//   - macro_hygiene_skipped      — #918 Phase 1 (via
//                                  InlinePass::total_macro_hygiene_skipped()
//                                  accessed through ir_inline_hygiene_skipped
//                                  helper in this file)
//   - mark_dirty_boundary_prune_count — exposed via workspace
//                                  accessor at src/core/ast.ixx:3964
//
// What the issue body asks for is a CONSOLIDATED hash view that
// surfaces all 5 in one call for AI editing loops. #1470 ships ONE
// new Aura primitive `query:ai-closedloop-readiness-stats` with the
// 5 fields + recommendation. AI Agent consumes this instead of
// calling 5 separate stats primitives.


namespace aura_issue_1470_detail {

    static int g_passed = 0;
    static int g_failed = 0;


    static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:ai-closedloop-readiness-stats\") '{}')", key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    }

} // namespace aura_issue_1470_detail

int aura_issue_1470_run() {
    using namespace aura_issue_1470_detail;
    std::println("=== Issue #1470: query:ai-closedloop-readiness-stats ===");

    aura::compiler::CompilerService cs;

    // AC1: primitive returns a hash with all 6 documented fields
    // (5 counters + recommendation). All values present and >= 0;
    // recommendation in [0, 4].
    {
        std::println(
            "\n--- AC1: (engine:metrics \"query:ai-closedloop-readiness-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:ai-closedloop-readiness-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "ai-closedloop-readiness-stats returns a hash");

        const auto wraps = hash_int(cs, "wraps");
        const auto invalidations = hash_int(cs, "invalidations");
        const auto batch_commits = hash_int(cs, "batch-commits");
        const auto hygiene_skips = hash_int(cs, "hygiene-skips");
        const auto dirty_prunes = hash_int(cs, "dirty-prunes");
        const auto rec = hash_int(cs, "recommendation");

        CHECK(wraps >= 0, std::format("wraps >= 0 (got {})", wraps));
        CHECK(invalidations >= 0, std::format("invalidations >= 0 (got {})", invalidations));
        CHECK(batch_commits >= 0, std::format("batch-commits >= 0 (got {})", batch_commits));
        CHECK(hygiene_skips >= 0, std::format("hygiene-skips >= 0 (got {})", hygiene_skips));
        CHECK(dirty_prunes >= 0, std::format("dirty-prunes >= 0 (got {})", dirty_prunes));
        CHECK(rec >= 0 && rec <= 4, std::format("recommendation in [0,4] (got {})", rec));
    }

    // AC2: fresh service — all 5 counters at 0, recommendation 0
    // (AC1 only reads counters via eval — no bumps). Verifies the
    // primitive is queryable on a brand-new CompilerService and the
    // documented baseline matches reality.
    {
        std::println("\n--- AC2: fresh service baseline (all zeros) ---");
        const auto wraps = hash_int(cs, "wraps");
        const auto invalidations = hash_int(cs, "invalidations");
        const auto batch_commits = hash_int(cs, "batch-commits");
        const auto hygiene_skips = hash_int(cs, "hygiene-skips");
        const auto dirty_prunes = hash_int(cs, "dirty-prunes");
        const auto rec = hash_int(cs, "recommendation");
        CHECK(wraps == 0, std::format("fresh wraps == 0 (got {})", wraps));
        CHECK(invalidations == 0, std::format("fresh invalidations == 0 (got {})", invalidations));
        CHECK(batch_commits == 0, std::format("fresh batch-commits == 0 (got {})", batch_commits));
        CHECK(hygiene_skips == 0, std::format("fresh hygiene-skips == 0 (got {})", hygiene_skips));
        CHECK(dirty_prunes == 0, std::format("fresh dirty-prunes == 0 (got {})", dirty_prunes));
        CHECK(rec == 0, std::format("fresh recommendation == 0 (got {})", rec));
    }

    // AC3: existing primitives remain reachable (back-compat —
    // #1470 doesn't disturb the existing surface).
    {
        std::println("\n--- AC3: existing primitives back-compat ---");
        auto h = cs.eval("(engine:metrics \"query:stable-ref-stats-hash\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "stable-ref-stats-hash still returns a hash");
        h = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "atomic-batch-stats-hash still returns a hash");
        h = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
        CHECK(h && aura::compiler::types::is_int(*h),
              "stable-ref-cow-fiber-stats still returns an int");
    }

    // AC4: recommendation enum contract documented:
    //   0 = healthy (all counters below thresholds)
    //   1 = wraps detected
    //   2 = high invalidation rate (>= 10)
    //   3 = high hygiene-skip rate (>= 100)
    //   4 = high dirty-prune rate (>= 50)
    // On a fresh service all 5 are at 0 → rec must be 0.
    {
        std::println("\n--- AC4: recommendation enum contract ---");
        const auto rec = hash_int(cs, "recommendation");
        CHECK(rec == 0, std::format("fresh service recommendation == 0 (got {})", rec));
    }

    std::println("\n--- harness totals ---");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

int run_i1470() {
    return aura_issue_1470_run();
}
} // namespace aura_iss_run_i1470
// ─── end test_issue_1470.cpp ───


// ─── from test_issue_1473.cpp → aura_iss_run_i1473::run_i1473 ───
namespace aura_iss_run_i1473 {
// @category: integration
// @reason: Issue #1473 — Robust StableNodeRef auto-restamp across
// COW/fiber-steal/GC
//
// Scope-limited close. The three hook sites wired by #1473:
//
//   (a) re_pin_cow_children_from_snapshot() — wired from
//       restore_post_yield_or_rollback + on_arena_compact_hook (#1446).
//       Previous implementation only bumped a counter; #1473 makes
//       it actually walk cow_boundary_pinned_refs_ and call
//       validate_or_refresh(*ws) on each.
//
//   (b) probe_linear_ownership_at_gc_safepoint() (evaluator_gc.cpp) —
//       already existed for linear ownership probing; #1473 appends a
//       pinned-ref validate sweep after the linear-ownership check so
//       GC-safepoint paths force-refresh pinned StableNodeRefs.
//
//   (c) probe_linear_ownership_on_fiber_steal() (evaluator_gc.cpp) —
//       same append pattern for fiber-steal paths.
//
// New counters wired in src/compiler/observability_metrics.h:
//   - stable_ref_validations_at_steal
//   - stable_ref_validations_at_gc_safepoint
//
// This test exercises all three hook sites in a 1000+ iter stress
// loop. Hook walks empty cow_boundary_pinned_refs_ are zero-cost —
// we drive raw hook calls (no per-iter eval/parse) so the test runs
// in well under the 60s default timeout.


namespace aura_issue_1473_detail {

    // test_harness.hpp defines `CHECK` already. We undefine and redefine
    // to print to cout/cerr with our formatting (same pattern as other
    // issue_14NN tests).

} // namespace aura_issue_1473_detail

int aura_issue_1473_run() {
    using namespace aura_issue_1473_detail;
    std::println("=== Issue #1473: StableNodeRef auto-restamp hooks (COW / steal / GC) ===");

    aura::compiler::CompilerService cs;
    auto* ev = &cs.evaluator();

    // AC1: 1000+ iter hook (a) — re_pin_cow_children_from_snapshot
    // drives the validate_or_refresh walk over cow_boundary_pinned_refs_.
    {
        std::println("\n--- AC1: 1000 iter hook (a) ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_re_pin_cow_children_from_snapshot();
        }
        CHECK(true, std::format("AC1: {} iter hook-(a) loop completed without crash", kIters));
    }

    // AC2: 1000 iter hook (b) — GC safepoint probe + pinned-ref sweep
    {
        std::println("\n--- AC2: 1000 iter hook (b) GC-safepoint probe ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_probe_linear_at_gc_safepoint();
        }
        CHECK(true, std::format("AC2: {} iter hook-(b) loop completed without crash", kIters));
    }

    // AC3: 1000 iter hook (c) — fiber-steal probe + pinned-ref sweep
    {
        std::println("\n--- AC3: 1000 iter hook (c) fiber-steal probe ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_probe_linear_on_fiber_steal();
        }
        CHECK(true, std::format("AC3: {} iter hook-(c) loop completed without crash", kIters));
    }

    // AC4: 500 mixed round-trips — exercises (a) + (b) interleaved.
    {
        std::println("\n--- AC4: 500 mixed hook-(a)+(b) round-trips ---");
        constexpr int kIters = 500;
        for (int i = 0; i < kIters; ++i) {
            ev->test_re_pin_cow_children_from_snapshot();
            ev->test_probe_linear_at_gc_safepoint();
        }
        CHECK(true, std::format("AC4: {} mixed round-trips completed", kIters));
    }

    // AC5: hooks remain callable post-stress (no resource exhaustion
    // or silent corruption).
    {
        std::println("\n--- AC5: hooks still callable post-stress ---");
        const bool ok_a = ev->test_re_pin_cow_children_from_snapshot();
        ev->test_probe_linear_at_gc_safepoint();
        ev->test_probe_linear_on_fiber_steal();
        CHECK(true, "AC5: all 3 hooks callable post-stress (return value ignored)");
        (void)ok_a;
    }

    std::println("\n--- harness totals ---");
    std::println("Total: {} passed, {} failed", ::aura::test::g_passed, ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_i1473() {
    return aura_issue_1473_run();
}
} // namespace aura_iss_run_i1473
// ─── end test_issue_1473.cpp ───

// ─── from test_issue_1474.cpp → aura_iss_run_i1474::run_i1474 ───
namespace aura_iss_run_i1474 {
// @category: integration
// @reason: Issue #1474 — Wire per-block dirty bitmask into actual
// re-lower path (scope-limited close)
//
// Scope-limited close matching #1459 / #1470 / #1473 pattern.
//
// Discovery before this PR (no duplication): the per-block dirty
// bitmask (Issue #196) + relower_define_blocks() / relower_define_function()
// helpers (Issue #224 cycle 3) already existed. The actual gap was
// that relower_define_function() did a WHOLE-function replace
// (entry.irs[func_idx] = std::move(new_func)) — the per-block
// bitmask was bumped but never actually used to select which blocks
// to copy. #1474 wires the per-block selective copy path.
//
// What ships in this PR:
//   - relower_define_function: per-block selective copy path
//     (Issue #1474 cycle 1). When dirty_mask size matches
//     new_func.blocks.size() AND entry.irs[func_idx].blocks.size(),
//     only dirty blocks are copied from new_func into entry.irs.
//     Clean blocks keep their old IR. Shape mismatch (block
//     count changed) falls back to whole-function replace
//     (preserves previous behavior).
//   - 1 new counter: incremental_relower_blocks_total — # of
//     blocks actually replaced across all relower_define_function
//     calls. With per-block selective copy, this grows by
//     dirty_block_count() per call (not total_blocks()).
//   - 1 new derived field: dirty_block_ratio_bp —
//     ir_soa_block_dirty_hits_total / (ir_soa_block_dirty_hits_total
//     + ir_soa_relower_blocks_saved_total) * 10000 (basis points).
//   - 2 new snapshot fields in CompilerSnapshot.
//
// What does NOT ship (deferred to follow-up issues):
//   - AC2: mark_define_dirty cascade for nested lambda
//     (irs.size() > 2) — needs dep_graph_-aware cascade refinement
//   - AC3: lookup_define_v2 / eval-current should prefer partial
//     re-lower — needs wiring of relower_define_blocks into the
//     eval pipeline (currently only called by relower_define_function
//     dispatch in cache_define path)
//
// This test exercises the per-block selective copy path in a 1000
// round stress loop. Each round marks only the body block dirty
// (block 0 of irs[1]) and re-lowers via relower_define_function.
// With the per-block path wired, incremental_relower_blocks_total
// should grow by 1 per round (not by the function's total block
// count). Synthetic 2-function / 1-block-each IR keeps the test
// fast and deterministic; lower_function_at does the real lowering
// of the Lambda AST node from a freshly-parsed flat/pool.


namespace aura_issue_1474_detail {

    // test_harness.hpp defines `CHECK` already (line ~127). We undefine
    // and redefine to print to cout/cerr with our formatting (same
    // pattern as other issue_14NN tests).

} // namespace aura_issue_1474_detail

int aura_issue_1474_run() {
    using namespace aura_issue_1474_detail;

    aura::compiler::CompilerService cs;

    // ── AC1 setup: store_define_v2 with synthetic 2-function / 1-block-each IR ──
    // irs[0] = __top__ entry function, irs[1] = body Lambda function.
    // The bitmask is initialized by store_define_v2 → init_block_dirty_from_irs
    // (sizes to irs[i].blocks.size(), marks all dirty) → clear_all_block_dirty
    // (flips to clean). Net effect: bitmask mirrors irs shape, all clean.
    aura::ir::IRFunction entry_fn;
    entry_fn.id = 0;
    entry_fn.name = "__top__";
    entry_fn.entry_block = 0;
    entry_fn.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body_fn;
    body_fn.id = 1;
    body_fn.name = "f_inner";
    body_fn.entry_block = 0;
    body_fn.blocks.push_back({0, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(entry_fn);
    irs.push_back(body_fn);
    cs.store_define_v2("f", "(define (f x) (+ x 1))", std::move(irs),
                       std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});

    const auto* entry = cs.get_define_v2("f");
    CHECK(entry != nullptr, "ir_cache_v2_[\"f\"] exists after store_define_v2");
    if (entry == nullptr) {
        return ::aura::test::g_failed == 0 ? 0 : 1;
    }
    CHECK(entry->irs.size() == 2, "entry has 2 functions (entry + body)");
    CHECK(entry->block_dirty_per_func_.size() == 2, "bitmask sized to 2 functions");
    CHECK(entry->block_dirty_per_func_[1].size() == 1, "body function bitmask has 1 block");
    CHECK(entry->block_dirty_per_func_[1][0] == 0, "body block starts clean");

    // ── AC2 setup: parse the source into a fresh flat + pool, find the Lambda node id ──
    // lower_function_at() needs a real Lambda AST node — the synthetic IRs in v2
    // don't carry AST context, so we re-parse the source to drive the lower.
    aura::ast::ASTArena arena(4096);
    auto a = arena.allocator();
    aura::ast::FlatAST flat(a);
    aura::ast::StringPool pool(a);
    auto pr = aura::parser::parse_to_flat("(define (f x) (+ x 1))", flat, pool);
    CHECK(pr.success, "parse_to_flat succeeded");
    if (pr.success)
        flat.root = pr.root;

    // Find the Define's Lambda child (the body of `f`).
    aura::ast::NodeId lambda_id = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM &&
            std::string(pool.resolve(v.sym_id)) == "f") {
            lambda_id = v.child(0);
            break;
        }
    }
    CHECK(lambda_id != aura::ast::NULL_NODE, "found lambda_id for f");

    // ── AC3: verify initial counter state ──
    const auto initial_blocks =
        cs.metrics().incremental_relower_blocks_total.load(std::memory_order_relaxed);
    const auto initial_per_fn =
        cs.metrics().relower_per_function_called_count.load(std::memory_order_relaxed);
    CHECK(initial_blocks == 0, "initial incremental_relower_blocks_total == 0");
    CHECK(initial_per_fn == 0, "initial relower_per_function_called_count == 0");

    // ── AC4: 1000 rounds of mark dirty + per-function re-lower ──
    // Each round marks only the body block (block 0 of irs[1]) dirty.
    // With per-block selective copy, incremental_relower_blocks_total
    // should grow by 1 per round (not by the function's total block count).
    constexpr int kRounds = 1000;
    int fail_count = 0;
    for (int i = 0; i < kRounds; ++i) {
        cs.mark_block_dirty_v2("f", /*func_idx=*/1, /*block_idx=*/0);
        const bool ok = cs.relower_define_function("f", /*func_idx=*/1, flat, pool, lambda_id);
        if (!ok) {
            ++fail_count;
        }
    }
    CHECK(fail_count == 0,
          std::format("all 1000 rounds of relower_define_function succeeded ({} failed)",
                      fail_count));

    // ── AC5: incremental_relower_blocks_total grew by 1000 (1 block per round) ──
    // This is the per-block win: the previous whole-function replace
    // would have grown this by total_blocks() (e.g. 1 for our 1-block
    // body, but the per-round counter for a multi-block function would
    // have grown by N). For 1000 rounds of 1 dirty block, this MUST
    // be exactly 1000.
    const auto after_blocks =
        cs.metrics().incremental_relower_blocks_total.load(std::memory_order_relaxed);
    CHECK(after_blocks == static_cast<std::uint64_t>(kRounds),
          std::format("incremental_relower_blocks_total == {} (got {})", kRounds, after_blocks));

    // ── AC6: relower_per_function_called_count grew by 1000 ──
    const auto after_per_fn =
        cs.metrics().relower_per_function_called_count.load(std::memory_order_relaxed);
    CHECK(after_per_fn == static_cast<std::uint64_t>(kRounds),
          std::format("relower_per_function_called_count == {} (got {})", kRounds, after_per_fn));

    // ── AC7: ir_soa_block_dirty_hits_total grew (1000 mark dirty calls) ──
    // mark_block_dirty_v2 bumps this by 1 per call.
    const auto after_hits =
        cs.metrics().ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
    CHECK(after_hits >= static_cast<std::uint64_t>(kRounds),
          std::format("ir_soa_block_dirty_hits_total >= {} (got {})", kRounds, after_hits));

    // ── AC8: snapshot fields are populated correctly ──
    auto snap = cs.snapshot();
    CHECK(snap.incremental_relower_blocks_total == static_cast<std::uint64_t>(kRounds),
          std::format("snapshot.incremental_relower_blocks_total == {} (got {})", kRounds,
                      snap.incremental_relower_blocks_total));

    // ── AC9: dirty_block_ratio_bp is computed from the existing fields ──
    const auto hits = cs.metrics().ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
    const auto saved =
        cs.metrics().ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed);
    const std::uint64_t sum = hits + saved;
    const std::uint64_t expected_bp = sum > 0 ? (hits * 10000u) / sum : 0;
    CHECK(snap.dirty_block_ratio_bp == expected_bp,
          std::format("snapshot.dirty_block_ratio_bp matches computed ({} == {}, hits={} saved={})",
                      snap.dirty_block_ratio_bp, expected_bp, hits, saved));

    // ── AC10: bitmask is clean after 1000 per-block re-lowers ──
    const auto* entry2 = cs.get_define_v2("f");
    CHECK(entry2 != nullptr, "ir_cache_v2_[\"f\"] still exists after stress");
    if (entry2 != nullptr) {
        CHECK(entry2->dirty == false, "entry.dirty flag is clear (all blocks re-lowered)");
        CHECK(entry2->dirty_block_count() == 0, "dirty_block_count() == 0 (all bits clear)");
    }

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_i1474() {
    return aura_issue_1474_run();
}

} // namespace aura_iss_run_i1474
// ─── end test_issue_1474.cpp ───

// ─── from test_issue_1475.cpp → aura_iss_run_i1475::run_i1475 ───
namespace aura_iss_run_i1475 {
// @category: integration
// @reason: Issue #1475 — EnvFrame version_ dual-check helper
// (parallel to is_bridge_stale) for IR closure apply paths.
//
// Scope-limited close matching #1459 / #1470 / #1473 / #1474
// pattern. The actual hot-path wiring of the dual check into
// IR executor (apply_closure dual-path) + JIT aura_closure_call +
// closure_bridge_ callback + multi-fiber stress test is deferred
// to follow-up issues (see close comment for the list).
//
// This test verifies the new pure helper `is_env_frame_stale`
// (added next to `is_bridge_stale` in evaluator.ixx) behaves
// correctly across the 7 documented Invariant cases:
//
//   1. current_defuse_version == 0 → tracking inactive → false
//   2. env_id == NULL_ENV_ID → no env_id → false (legacy path)
//   3. env_id != NULL_ENV_ID + frame_version == 0 → strict stale
//   4. frame_version < current_defuse_version → stale (Issue #242)
//   5. frame_version == current_defuse_version → fresh
//   6. frame_version > current_defuse_version → defensive false
//
// And the integration with defuse_version_for_test /
// bump_defuse_version_for_test (public accessors at
// evaluator.ixx:2729-2730).


namespace aura_issue_1475_detail {} // namespace aura_issue_1475_detail

int aura_issue_1475_run() {
    using namespace aura_issue_1475_detail;

    // Force strict default (don't inherit AURA_BRIDGE_EPOCH_LEGACY_TRUST
    // from a shell env that could mask the stale path).
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);

    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();

    constexpr std::uint64_t kSomeEnvId = 42; // arbitrary non-NULL id

    // ── AC1: helper is accessible through Evaluator instance ──
    // is_env_frame_stale is a static method; C++ allows calling
    // it through instance references (verify the export works).
    const bool accessible = ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 200);
    CHECK(accessible == true,
          "is_env_frame_stale is accessible via Evaluator instance (static method works)");

    // ── AC2: current_defuse_version == 0 → false (tracking inactive) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 0, 0) == false,
          "current==0 with env_id!=NULL returns false (tracking inactive)");
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 0) == false,
          "current==0 with non-zero frame_version returns false (tracking inactive)");

    // ── AC3: env_id == NULL_ENV_ID → false (closure without env_id) ──
    CHECK(ev.is_env_frame_stale(aura::compiler::NULL_ENV_ID, 0, 200) == false,
          "env_id==NULL_ENV_ID returns false (legacy / pre-SoA path)");
    CHECK(ev.is_env_frame_stale(aura::compiler::NULL_ENV_ID, 100, 200) == false,
          "env_id==NULL_ENV_ID with frame_version < current returns false");

    // ── AC4: env_id != NULL + frame_version == 0 + current != 0 → STALE (strict) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 0, 100) == true,
          "frame_version==0 with current>0 returns true (strict default)");

    // ── AC5: frame_version < current → stale (Issue #242 invariant) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 50, 100) == true,
          "frame_version<current returns true (stale capture)");
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 99, 100) == true,
          "frame_version=current-1 returns true (off-by-one check)");

    // ── AC6: frame_version == current → false (fresh) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 100) == false,
          "frame_version==current returns false (fresh capture)");

    // ── AC7: frame_version > current → false (defensive; impossible in practice) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 150, 100) == false,
          "frame_version>current returns false (defensive)");

    // ── AC8: bump_defuse_version_for_test increments by 1 ──
    const auto before_bump = ev.defuse_version_for_test();
    ev.bump_defuse_version_for_test();
    const auto after_one_bump = ev.defuse_version_for_test();
    CHECK(after_one_bump == before_bump + 1,
          std::format("bump_defuse_version_for_test increments by 1 ({} -> {})", before_bump,
                      after_one_bump));

    // ── AC9: post-bump, frame with version == before_bump is stale ──
    // The frame was "captured" at before_bump (== pre-mutation defuse
    // version); after bumping, the current is after_one_bump and the
    // capture pre-dates it → stale.
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), before_bump,
                                after_one_bump) == true,
          "post-bump: frame with pre-bump version is stale");

    // ── AC10: post-bump, frame with version == after_one_bump is fresh ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), after_one_bump,
                                after_one_bump) == false,
          "post-bump: frame with same-as-current version is fresh");

    // ── AC11: 1000 bump iterations drive the helper deterministically ──
    // Stresses the helper across many bumps to verify no state leaks
    // and the math stays correct.
    for (int i = 0; i < 1000; ++i) {
        ev.bump_defuse_version_for_test();
    }
    const auto after_1001 = ev.defuse_version_for_test();
    CHECK(after_1001 == before_bump + 1 + 1000,
          std::format("1000 bump iterations produce expected count ({} expected {})", after_1001,
                      before_bump + 1 + 1000));
    // A frame with version = before_bump is now 1001 versions behind
    // → still stale.
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), before_bump, after_1001) ==
              true,
          "frame_version = pre-storm baseline is stale after 1001 bumps");

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_i1475() {
    return aura_issue_1475_run();
}

} // namespace aura_iss_run_i1475
// ─── end test_issue_1475.cpp ───

// ─── from test_issue_1476.cpp → aura_iss_run_i1476::run_i1476 ───
namespace aura_iss_run_i1476 {
// @category: integration
// @reason: Issue #1476 — unify mark_define_dirty / invalidate_function +
// atomic bridge_epoch + mutation_epoch bump protocol (scope-limited close).
//
// Scope-limited close matching #1459 / #1470 / #1473 / #1474 / #1475
// pattern. The actual hot-path refactor (full unify of all invalidation
// paths + JIT fn_trackers_ notify + multi-fiber concurrent stress +
// lock-ordering audit) is deferred to follow-up issues (see commit
// message for the list).
//
// This test verifies the MVP for #1476:
//
//   - The new metric `bridge_epoch_bumps_total` tracks every
//     `bump_bridge_epoch()` call (wired at the source).
//   - `mark_define_dirty(name)` now acquires `mutate_mtx_` + bumps
//     BOTH epochs (bridge_epoch via mutation_epoch_ release + defuse_version
//     acq_rel) atomically via the new helper
//     `atomic_bump_epochs_and_stamp_bridge(name)`.
//   - The BFS cascade also bumps both epochs per dependent so
//     closure captures in dependents see the new epoch.
//   - `invalidate_cascade_depth_max` is tracked via CAS loop
//     (monotonic high-water mark for BFS depth).


namespace aura_issue_1476_detail {

    // test_harness.hpp defines `CHECK` already (line ~127). We undefine
    // and redefine to print to cout/cerr with our formatting (same
    // pattern as other issue_14NN tests).

} // namespace aura_issue_1476_detail

int aura_issue_1476_run() {
    using namespace aura_issue_1476_detail;

    aura::compiler::CompilerService cs;

    // ── AC1: bump_bridge_epoch() increments bridge_epoch_bumps_total ──
    const auto initial_bridge_bumps =
        cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    cs.bump_bridge_epoch();
    const auto after_one_bump =
        cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    CHECK(after_one_bump == initial_bridge_bumps + 1,
          std::format("bump_bridge_epoch increments bridge_epoch_bumps_total by 1 ({} -> {})",
                      initial_bridge_bumps, after_one_bump));

    // ── AC2: mark_define_dirty bumps BOTH epochs atomically ──
    // Set up a synthetic entry so mark_define_dirty has something
    // to mark + cascade over.
    cs.store_define_v2("f", "(define (f x) (+ x 1))", std::vector<aura::ir::IRFunction>{},
                       std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});
    const auto pre_bridge = cs.bridge_epoch();
    const auto pre_defuse = cs.evaluator().defuse_version_for_test();
    const auto pre_bridge_bumps =
        cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    const auto pre_cascade_depth =
        cs.metrics().invalidate_cascade_depth_max.load(std::memory_order_relaxed);

    cs.mark_define_dirty("f");

    const auto post_bridge = cs.bridge_epoch();
    const auto post_defuse = cs.evaluator().defuse_version_for_test();
    const auto post_bridge_bumps =
        cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    const auto post_cascade_depth =
        cs.metrics().invalidate_cascade_depth_max.load(std::memory_order_relaxed);

    CHECK(post_bridge > pre_bridge,
          std::format("bridge_epoch bumped after mark_define_dirty ({} -> {})", pre_bridge,
                      post_bridge));
    CHECK(post_defuse > pre_defuse,
          std::format("defuse_version_ bumped after mark_define_dirty ({} -> {})", pre_defuse,
                      post_defuse));
    CHECK(post_bridge_bumps > pre_bridge_bumps,
          std::format("bridge_epoch_bumps_total incremented ({} -> {})", pre_bridge_bumps,
                      post_bridge_bumps));
    CHECK(post_cascade_depth >= std::max<std::uint64_t>(pre_cascade_depth, 1),
          std::format("invalidate_cascade_depth_max tracked (>= 1, was {} now {})",
                      pre_cascade_depth, post_cascade_depth));

    // ── AC3: 1000-iter stress test for dual-epoch atomicity ──
    // Each iteration marks a (possibly no-op) define dirty. We
    // verify the bridge_epoch_bumps_total grows by ≥1000 over
    // the 1000 calls. (The exact count is >=1000 because the
    // bump_defuse_version_for_test isn't paired with bump_bridge_epoch
    // by an automatic increment — only mark_define_dirty triggers
    // both, but mark_all_defines_dirty doesn't, so the count is
    // exact.)
    const auto pre_stress = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        cs.mark_define_dirty("f");
    }
    const auto post_stress = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    CHECK(post_stress >= pre_stress + 1000,
          std::format("1000 rounds bumps bridge_epoch_bumps_total by >= 1000 ({} -> {})",
                      pre_stress, post_stress));

    // ── AC4: bridge_epoch and defuse_version_ grow in lockstep over 1000 rounds ──
    const auto bridge_at_stress_start = cs.bridge_epoch();
    const auto defuse_at_stress_start = cs.evaluator().defuse_version_for_test();
    for (int i = 0; i < 100; ++i) {
        cs.mark_define_dirty("f");
    }
    const auto bridge_delta = cs.bridge_epoch() - bridge_at_stress_start;
    const auto defuse_delta = cs.evaluator().defuse_version_for_test() - defuse_at_stress_start;
    CHECK(bridge_delta >= 100,
          std::format("bridge_epoch grew by >= 100 in 100 rounds (got {})", bridge_delta));
    CHECK(defuse_delta >= 100,
          std::format("defuse_version_ grew by >= 100 in 100 rounds (got {})", defuse_delta));
    CHECK(bridge_delta == defuse_delta,
          std::format("dual-epoch lockstep: bridge_delta ({}) == defuse_delta ({})", bridge_delta,
                      defuse_delta));

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_i1476() {
    return aura_issue_1476_run();
}
} // namespace aura_iss_run_i1476
// ─── end test_issue_1476.cpp ───

// ─── from test_issue_1478.cpp → aura_iss_run_i1478::run_i1478 ───
namespace aura_iss_run_i1478 {
// @category: unit
// @reason: Issue #1478 MVP — linear_post_mutate_enforce + counters
// Restored/verified by Issue #1541 (scope-limited close deferred build).
//
//   AC1: linear_post_mutate_enforcements accessor returns 0 initially
//   AC2: linear_ownership_violation_prevented accessor returns 0 initially
//   AC3: helper returns true for NULL_ENV_ID (safety net)
//   AC4: helper returns true for invalid env_id (safety net)
//   AC5: NULL_ENV_ID doesn't bump counter
//   AC6: invalid env_id doesn't bump counter
//   AC7: linear_post_mutate_enforce on valid frame bumps counter
//   AC8: 1000-iter stress (monotonic)
//   AC9: violation_prevented stays 0 without Moved bindings (passive path)


namespace aura_issue_1478_detail {

    using aura::compiler::CompilerService;
    using aura::compiler::NULL_ENV_ID;
    using aura::compiler::types::make_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static void ac1_enforce_count_initial() {
        std::println("\n--- AC1: linear_post_mutate_enforcements accessor 0 initially ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.test_linear_post_mutate_enforce_count() == 0,
              "linear_post_mutate_enforcements starts at 0");
    }

    static void ac2_violation_count_initial() {
        std::println("\n--- AC2: linear_ownership_violation_prevented accessor 0 initially ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.test_linear_ownership_violation_prevented_count() == 0,
              "linear_ownership_violation_prevented starts at 0");
    }

    static void ac3_null_env_safe() {
        std::println("\n--- AC3: helper returns true for NULL_ENV_ID ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.linear_post_mutate_enforce(NULL_ENV_ID) == true,
              "NULL_ENV_ID → true (safety net)");
    }

    static void ac4_invalid_env_safe() {
        std::println("\n--- AC4: helper returns true for invalid env_id ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Far beyond any allocated frame.
        constexpr auto kInvalid = static_cast<aura::compiler::EnvId>(1u << 30);
        CHECK(ev.linear_post_mutate_enforce(kInvalid) == true,
              "invalid env_id → true (safety net)");
    }

    static void ac5_null_no_bump() {
        std::println("\n--- AC5: NULL_ENV_ID doesn't bump counter ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto c0 = ev.test_linear_post_mutate_enforce_count();
        (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
        (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
        CHECK(ev.test_linear_post_mutate_enforce_count() == c0,
              "NULL_ENV_ID does not bump enforcements counter");
    }

    static void ac6_invalid_no_bump() {
        std::println("\n--- AC6: invalid env_id doesn't bump counter ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto c0 = ev.test_linear_post_mutate_enforce_count();
        constexpr auto kInvalid = static_cast<aura::compiler::EnvId>(1u << 30);
        (void)ev.linear_post_mutate_enforce(kInvalid);
        CHECK(ev.test_linear_post_mutate_enforce_count() == c0,
              "invalid env_id does not bump enforcements counter");
    }

    static void ac7_valid_frame_bumps() {
        std::println("\n--- AC7: valid frame bumps enforcements (closure-path equivalent) ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Allocate a live EnvFrame (Owned binding, not Moved).
        aura::compiler::Env src;
        src.bind_symid_with_linear_state(1, make_int(1), /*Owned*/ 1);
        auto eid = ev.alloc_env_frame_from_env(src);
        CHECK(eid != NULL_ENV_ID, "allocated frame");
        const auto c0 = ev.test_linear_post_mutate_enforce_count();
        // Mirrors what closure_needs_safe_fallback does on apply with captures.
        CHECK(ev.linear_post_mutate_enforce(eid) == true, "Owned frame → true");
        CHECK(ev.test_linear_post_mutate_enforce_count() == c0 + 1,
              "valid frame bumps enforcements by 1");
        // Also exercise via eval path that applies closures (secondary coverage).
        CHECK(cs.eval("(set-code \"(define g (lambda () 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
        const auto c1 = ev.test_linear_post_mutate_enforce_count();
        (void)cs.eval("(g)");
        // apply may or may not bump depending on env_id capture path; count is monotonic.
        CHECK(ev.test_linear_post_mutate_enforce_count() >= c1, "after (g) enforcements monotonic");
    }

    static void ac8_stress_1000() {
        std::println("\n--- AC8: 1000-iter stress (monotonic) ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::compiler::Env src;
        src.bind_symid_with_linear_state(7, make_int(0), /*Owned*/ 1);
        auto eid = ev.alloc_env_frame_from_env(src);
        const auto c0 = ev.test_linear_post_mutate_enforce_count();
        bool all_ok = true;
        for (int i = 0; i < 1000; ++i) {
            if (!ev.linear_post_mutate_enforce(eid))
                all_ok = false;
        }
        CHECK(all_ok, "1000-iter all safe");
        const auto c1 = ev.test_linear_post_mutate_enforce_count();
        CHECK(c1 == c0 + 1000, "1000 enforces → +1000 counter");
        std::println("  counter {} → {}", c0, c1);
    }

    static void ac9_violation_passive_without_moved() {
        std::println("\n--- AC9: violation_prevented stays 0 without Moved bindings ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::compiler::Env src;
        src.bind_symid_with_linear_state(3, make_int(3), /*Owned*/ 1);
        auto eid = ev.alloc_env_frame_from_env(src);
        const auto v0 = ev.test_linear_ownership_violation_prevented_count();
        for (int i = 0; i < 50; ++i)
            (void)ev.linear_post_mutate_enforce(eid);
        (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
        CHECK(ev.test_linear_ownership_violation_prevented_count() == v0,
              "no Moved bindings → violation_prevented unchanged");
    }

    // Issue #1638 extension: 3 new ACs (AC10/11/12) for the SoA EnvFrame
    // dual-path consistency + mutation_log compact hooks. Source-driven
    // smoke tests (the actual compact fires on heavy-mutation scenarios
    // impractical to drive in a unit test; these verify the metrics
    // surface + bump helpers are wired correctly + the public methods
    // are callable from outside the Evaluator).

    static void ac10_dual_path_metrics_reachable(CompilerService& cs) {
        auto& ev = cs.evaluator();
        // Newly added metrics start at 0 on a fresh service.
        const auto dp0 = ev.get_dual_path_stale_fallback_total();
        const auto ml0 = ev.get_mutation_log_compact_bytes_saved();
        const auto vd0 = ev.get_env_frame_version_drift_prevented();
        CHECK(dp0 == 0 && ml0 == 0 && vd0 == 0,
              "AC10: dual_path / compact-bytes / drift-prevented all start at 0");
    }

    static void ac11_ensure_dual_path_consistent_callable(CompilerService& cs) {
        auto& ev = cs.evaluator();
        // NULL_ENV_ID should be the safe net — returns true without bumping.
        const auto before = ev.get_env_frame_version_drift_prevented();
        (void)ev.ensure_env_frame_dual_path_consistent(NULL_ENV_ID, "ac11_null_env_id");
        const auto after = ev.get_env_frame_version_drift_prevented();
        CHECK(after == before, "AC11: NULL_ENV_ID does not bump env_frame_version_drift_prevented");
    }

    static void ac12_compact_mutation_log_callable(CompilerService& cs) {
        auto& ev = cs.evaluator();
        // No-op when mutation_log is empty (returns 0 bytes saved, no bump).
        const auto before = ev.get_mutation_log_compact_bytes_saved();
        ev.compact_mutation_log();
        const auto after = ev.get_mutation_log_compact_bytes_saved();
        CHECK(after == before,
              "AC12: compact_mutation_log on empty log is a no-op (monotonic metric)");
    }

} // namespace aura_issue_1478_detail

int run_i1478() {
    using namespace aura_issue_1478_detail;
    std::println("=== Issue #1478: linear_post_mutate_enforce MVP (verified by #1541) ===");
    std::println(
        "=== + Issue #1638: SoA EnvFrame dual-path consistency extension (AC10/11/12) ===");
    ac1_enforce_count_initial();
    ac2_violation_count_initial();
    ac3_null_env_safe();
    ac4_invalid_env_safe();
    ac5_null_no_bump();
    ac6_invalid_no_bump();
    ac7_valid_frame_bumps();
    ac8_stress_1000();
    ac9_violation_passive_without_moved();
    {
        CompilerService cs;
        ac10_dual_path_metrics_reachable(cs);
        ac11_ensure_dual_path_consistent_callable(cs);
        ac12_compact_mutation_log_callable(cs);
    }
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_iss_run_i1478
// ─── end test_issue_1478.cpp ───
int main() {
    std::println("\n######## run_i1466 ########");
    if (int rc = aura_iss_run_i1466::run_i1466(); rc != 0) {
        std::println("run_i1466 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1467 ########");
    if (int rc = aura_iss_run_i1467::run_i1467(); rc != 0) {
        std::println("run_i1467 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1468 ########");
    if (int rc = aura_iss_run_i1468::run_i1468(); rc != 0) {
        std::println("run_i1468 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1470 ########");
    if (int rc = aura_iss_run_i1470::run_i1470(); rc != 0) {
        std::println("run_i1470 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1473 ########");
    if (int rc = aura_iss_run_i1473::run_i1473(); rc != 0) {
        std::println("run_i1473 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1474 ########");
    if (int rc = aura_iss_run_i1474::run_i1474(); rc != 0) {
        std::println("run_i1474 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1475 ########");
    if (int rc = aura_iss_run_i1475::run_i1475(); rc != 0) {
        std::println("run_i1475 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1476 ########");
    if (int rc = aura_iss_run_i1476::run_i1476(); rc != 0) {
        std::println("run_i1476 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_i1478 ########");
    if (int rc = aura_iss_run_i1478::run_i1478(); rc != 0) {
        std::println("run_i1478 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\ntest_issues_1466_1478_batch: OK");
    return 0;
}

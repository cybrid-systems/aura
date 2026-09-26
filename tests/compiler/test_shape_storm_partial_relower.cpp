// @category: unit
// @reason: Issue #2212 — link ShapeProfiler deopt-storm / StormLevel Shape
// bit to partial-relower threshold (prefer partial under shape churn).
//
//   AC1: When StormLevel has Shape bit, partial is preferred for a wider
//        dirty_count range (thr * 2).
//   AC2: Global-only storm does NOT force partial preference (still force
//        full via #2190); reemit throttle path unchanged.
//   AC3: query:incremental-relower-stats / policy-stats surface
//        partial_relower_under_shape_storm_total + schema-2212.
//   AC4: Existing StormLevel wiring retained (#2190 / #2094 lineage).

#include "test_harness.hpp"
#include "compiler/hot_update_registry.hh"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;
import aura.compiler.dirty_propagation;

namespace {

using aura::compiler::apply_shape_storm_partial_preference;
using aura::compiler::CompilerService;
using aura::compiler::decide_workload_adaptive_partial_relower;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::kShapeStormEmptyPersistIssue;
using aura::compiler::kStormLevelGlobal;
using aura::compiler::kStormLevelShape;
using aura::compiler::partial_relower_storm_forced_full_total_atomic;
using aura::compiler::partial_relower_threshold_is_forced;
using aura::compiler::partial_relower_under_shape_storm_total_atomic;
using aura::compiler::prefer_partial_under_shape_storm;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::shape_storm_widened_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::should_partial_relower_storm_aware;
using aura::compiler::storm_level_has_global;
using aura::compiler::storm_level_has_shape;
using aura::compiler::dirty::reset_residual_castop_persist_for_test;
using aura::compiler::dirty::residual_castop_persist_size;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" std::uint8_t aura_hot_update_current_storm_level(void);
extern "C" void aura_hot_update_note_deopt(void);
extern "C" void aura_hot_update_set_deopt_storm_threshold(std::uint64_t, std::uint64_t);
extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void);
extern "C" void aura_hot_update_set_shape_storm_active(int);

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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void trip_global_storm() {
    aura_hot_update_set_deopt_storm_threshold(5, 1000);
    for (int i = 0; i < 10; ++i)
        aura_hot_update_note_deopt();
}

static void clear_storm() {
    aura_hot_update_reset_deopt_storm_state_for_test();
    aura_hot_update_set_shape_storm_active(0);
}

static void ac1_shape_widens_partial_window() {
    std::println("\n--- AC1: Shape bit widens partial dirty_count window ---");
    reset_partial_relower_threshold_for_test();
    clear_storm();
    CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold, "base thr 8");
    // Without storm: dirty=8 is full
    CHECK(!should_partial_relower(8), "pure thr: 8 → full");
    CHECK(!should_partial_relower_storm_aware(8), "None+8 → full");

    aura_hot_update_set_shape_storm_active(1);
    CHECK(storm_level_has_shape(), "Shape bit set");
    CHECK(!storm_level_has_global(), "Global off");
    CHECK((aura_hot_update_current_storm_level() & kStormLevelShape) != 0, "sl Shape");

    const auto wide = shape_storm_widened_threshold(get_partial_relower_threshold());
    CHECK(wide == 16, "2× thr = 16 (default 8)");
    CHECK(should_partial_relower_storm_aware(8), "AC1: Shape+8 → partial (was full)");
    CHECK(should_partial_relower_storm_aware(15), "AC1: Shape+15 → partial");
    CHECK(!should_partial_relower_storm_aware(16), "AC1: Shape+16 → full past 2×");
    // Pure thr still unchanged
    CHECK(!should_partial_relower(8), "AC1: pure thr still full for 8");
    CHECK(prefer_partial_under_shape_storm(10, /*base*/ false),
          "AC1: prefer helper flips false→true at dirty 10");

    const auto m0 = partial_relower_under_shape_storm_total_atomic().load();
    CHECK(should_partial_relower_storm_aware(9), "metric path partial");
    CHECK(partial_relower_under_shape_storm_total_atomic().load() > m0,
          "AC1: under-shape-storm metric advanced");
    clear_storm();
}

static void ac2_global_only_no_partial_prefer() {
    std::println("\n--- AC2: Global-only does not prefer partial ---");
    reset_partial_relower_threshold_for_test();
    clear_storm();
    trip_global_storm();
    CHECK(storm_level_has_global(), "Global bit");
    CHECK(!storm_level_has_shape(), "Shape off under Global-only");
    const auto s0 = partial_relower_under_shape_storm_total_atomic().load();
    const auto f0 = partial_relower_storm_forced_full_total_atomic().load();
    // Global still force-full (#2190); no shape preference metric bump.
    CHECK(!should_partial_relower_storm_aware(1), "AC2: Global+1 → full");
    CHECK(!should_partial_relower_storm_aware(8), "AC2: Global+8 → full");
    CHECK(!should_partial_relower_storm_aware(15), "AC2: Global+15 → full");
    CHECK(partial_relower_storm_forced_full_total_atomic().load() >= f0 + 3,
          "AC2: forced_full advances under Global");
    CHECK(partial_relower_under_shape_storm_total_atomic().load() == s0,
          "AC2: under-shape-storm metric not bumped for Global-only");
    // Both: Shape|Global → Global wins (force full) even with Shape widen.
    aura_hot_update_set_shape_storm_active(1);
    CHECK((aura_hot_update_current_storm_level() & kStormLevelShape) != 0, "Both Shape");
    CHECK((aura_hot_update_current_storm_level() & kStormLevelGlobal) != 0, "Both Global");
    CHECK(!should_partial_relower_storm_aware(8), "AC2: Both+8 → full (Global wins)");
    clear_storm();
}

static void ac3_query_schema_2212() {
    std::println("\n--- AC3: query schema-2212 + residual keys ---");
    reset_partial_relower_threshold_for_test();
    clear_storm();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "query:incremental-relower-stats", "schema-2212") == 2212,
          "relower schema-2212");
    CHECK(href(cs, "query:incremental-relower-stats", "issue-2212") == 2212, "relower issue-2212");
    CHECK(href(cs, "query:incremental-relower-stats", "shape-storm-partial-prefer-wired") == 1,
          "relower wired");
    CHECK(href(cs, "query:incremental-relower-stats", "partial-relower-under-shape-storm-total") >=
              0,
          "relower kebab key");
    CHECK(href(cs, "query:incremental-relower-stats", "partial_relower_under_shape_storm_total") >=
              0,
          "relower snake key");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "schema-2212") == 2212,
          "policy schema-2212");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "shape-storm-partial-prefer-wired") ==
              1,
          "policy wired");
    CHECK(href(cs, "query:incremental-relower-policy-stats",
               "partial-relower-under-shape-storm-total") >= 0,
          "policy residual key");

    // Exercise then re-read
    aura_hot_update_set_shape_storm_active(1);
    const auto a0 = partial_relower_under_shape_storm_total_atomic().load();
    CHECK(should_partial_relower_storm_aware(10), "exercise shape prefer");
    const auto a1 = partial_relower_under_shape_storm_total_atomic().load();
    CHECK(a1 > a0, "atomic advanced");
    const auto q =
        href(cs, "query:incremental-relower-stats", "partial_relower_under_shape_storm_total");
    CHECK(q >= static_cast<std::int64_t>(a1), "query exposes metric ≥ atomic");
    clear_storm();
}

static void ac4_lineage_and_source() {
    std::println("\n--- AC4: #2190/#2094 lineage + source wiring ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!pure.empty() && pure.find("Issue #2212") != std::string::npos, "ir_cache_pure #2212");
    CHECK(pure.find("prefer_partial_under_shape_storm") != std::string::npos, "prefer helper");
    CHECK(pure.find("partial_relower_under_shape_storm_total") != std::string::npos, "metric");
    CHECK(pure.find("apply_shape_storm_partial_preference") != std::string::npos,
          "workload preference");
    CHECK(pure.find("kAdaptiveReasonShapeStormPartial") != std::string::npos, "reason bit");
    CHECK(svc.find("apply_shape_storm_partial_preference") != std::string::npos,
          "service consult wires #2212");
    CHECK(obs.find("schema-2212") != std::string::npos, "query schema-2212");
    // Lineage: #2190 Global gate still present
    CHECK(pure.find("apply_partial_relower_storm_gate") != std::string::npos,
          "#2190 gate retained");
    CHECK(pure.find("Issue #2190") != std::string::npos, "#2190 cite retained");
    // Forced thr still respected as base of widen
    reset_partial_relower_threshold_for_test();
    clear_storm();
    set_partial_relower_threshold(4);
    aura_hot_update_set_shape_storm_active(1);
    CHECK(shape_storm_widened_threshold(4) == 8, "forced thr 4 → wide 8");
    CHECK(should_partial_relower_storm_aware(7), "Shape+7 partial at forced thr 4 (wide 8)");
    CHECK(!should_partial_relower_storm_aware(8), "Shape+8 full past wide 8");
    clear_storm();
    reset_partial_relower_threshold_for_test();
}

static void ac3070_hysteresis_and_forced_thr() {
    std::println("\n--- #3070: storm-exit hysteresis + forced-thr under storm ---");
    reset_partial_relower_threshold_for_test();
    clear_storm();

    // Shape → None with residual deopt window: keep force-full for a cooldown.
    aura_hot_update_set_deopt_storm_threshold(1000, 10000);
    for (int i = 0; i < 4; ++i)
        aura_hot_update_note_deopt();
    aura_hot_update_set_shape_storm_active(1);
    CHECK(should_partial_relower_storm_aware(3), "3070: Shape+3 still partial");
    aura_hot_update_set_shape_storm_active(0);
    CHECK(!storm_level_has_shape() && !storm_level_has_global(), "3070: now None");
    const auto f0 = partial_relower_storm_forced_full_total_atomic().load();
    CHECK(!should_partial_relower_storm_aware(3), "3070 AC1: cooldown force-full after Shape→None");
    CHECK(partial_relower_storm_forced_full_total_atomic().load() > f0,
          "3070 AC1: forced_full advances on cooldown");
    // After cooldown window (8 consults including the one above), None is partial.
    for (int i = 0; i < 10; ++i)
        (void)should_partial_relower_storm_aware(3);
    CHECK(should_partial_relower_storm_aware(3), "3070 AC1: after cooldown, None is partial");

    // Test reset drops cooldown immediately (existing None ACs).
    aura_hot_update_set_shape_storm_active(1);
    for (int i = 0; i < 4; ++i)
        aura_hot_update_note_deopt();
    aura_hot_update_set_shape_storm_active(0);
    clear_storm();
    CHECK(should_partial_relower_storm_aware(3), "3070 AC1: reset clears hysteresis");

    // Forced-wide thr cannot stay wide under Global (adapt from default).
    reset_partial_relower_threshold_for_test();
    clear_storm();
    set_partial_relower_threshold(32);
    CHECK(partial_relower_threshold_is_forced(), "3070: forced");
    trip_global_storm();
    auto d = decide_workload_adaptive_partial_relower(7, 10, /*deopt_win*/ 10,
                                                      /*deopt_thr*/ 5, /*storm*/ true);
    CHECK(d.effective_threshold < 32, "3070 AC2: forced-wide not kept under Global");
    CHECK(d.effective_threshold <= kDefaultPartialRelowerThreshold,
          "3070 AC2: adapt from default base");
    clear_storm();
    reset_partial_relower_threshold_for_test();
}

// ── Issue #3986: Shape-flip of adaptive-full + empty persist → production full ──
//   AC1: Shape-only + adaptive would-be-full + empty persist → peel full
//   AC2: Shape-only + adaptive partial is not a flip (persist attributed
//        stays partial — live in test_dead_coercion_dirty_cone)
//   AC3: Both/Global still force full (existing gate)
//   AC5: storm exit still clears partial_relower_threshold_forced
static void ac3986_shape_flip_empty_persist_forces_full() {
    std::println("\n--- #3986: Shape-flip + empty persist → production full ---");
    reset_partial_relower_threshold_for_test();
    clear_storm();
    reset_residual_castop_persist_for_test();

    CHECK(kShapeStormEmptyPersistIssue == 3986, "3986: issue stamp");

    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(pure.find("shape_flipped_full_to_partial") != std::string::npos,
          "3986 AC1: decision latches Shape flip");
    CHECK(pure.find("kShapeStormEmptyPersistIssue") != std::string::npos, "3986: stamp in pure");
    CHECK(svc.find("Issue #3986") != std::string::npos, "3986 AC1: consult cites #3986");
    CHECK(svc.find("residual_castop_persist_size() == 0") != std::string::npos,
          "3986 AC1: persist-empty unknown cone");
    CHECK(svc.find("production_hard_face_active()") != std::string::npos,
          "3986 AC1: production-only persist consult");
    CHECK(svc.find("shape_flipped_full_to_partial") != std::string::npos,
          "3986 AC1: peel sees flip flag");
    CHECK(dirty.find("Issue #3986") != std::string::npos, "3986: dirty-path distinguisher");
    CHECK(svc.find("schema-3986") == std::string::npos, "3986: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3986.cpp").empty(), "3986: no test_issue_3986.cpp");
    CHECK(read_file("docs/design/3986-shape-storm-empty-persist.md").empty(),
          "3986: no docs/design");

    // Pure helper: Shape still widens (#2212). Flip flag is the 3986 latch;
    // persist consult lives in service, not ir_cache_pure.
    {
        auto full = decide_workload_adaptive_partial_relower(/*dirty*/ 8, /*total*/ 8,
                                                             /*win*/ 0, /*st*/ 0, /*storm*/ false);
        CHECK(!full.want_partial, "3986 AC1: adaptive 8/8 is full");
        CHECK(!full.shape_flipped_full_to_partial, "3986 AC1: no flip before Shape");
        aura_hot_update_set_shape_storm_active(1);
        apply_shape_storm_partial_preference(full, 8);
        CHECK(full.want_partial, "3986 AC1: Shape flips adaptive-full → partial");
        CHECK(full.shape_flipped_full_to_partial, "3986 AC1: flip latched");
        CHECK(should_partial_relower_storm_aware(8),
              "3986: pure storm-aware still prefers partial (#2212)");
        clear_storm();

        auto already = decide_workload_adaptive_partial_relower(3, 10, 0, 0, false);
        CHECK(already.want_partial, "3986 AC2: adaptive 3 is partial");
        aura_hot_update_set_shape_storm_active(1);
        apply_shape_storm_partial_preference(already, 3);
        CHECK(already.want_partial, "3986 AC2: stays partial");
        CHECK(!already.shape_flipped_full_to_partial, "3986 AC2: no flip when already partial");
        clear_storm();
    }

    // AC3: Both/Global still force full after Shape widen.
    {
        aura_hot_update_set_shape_storm_active(1);
        aura_hot_update_set_deopt_storm_threshold(5, 1000);
        for (int i = 0; i < 10; ++i)
            aura_hot_update_note_deopt();
        CHECK((aura_hot_update_current_storm_level() & kStormLevelGlobal) != 0, "3986 AC3: Global");
        CHECK((aura_hot_update_current_storm_level() & kStormLevelShape) != 0, "3986 AC3: Shape");
        CHECK(!should_partial_relower_storm_aware(8), "3986 AC3: Both+8 → full (Global wins)");
        clear_storm();
    }

    // AC5: storm exit still clears partial_relower_threshold_forced.
    {
        apply_production_audit_defaults();
        set_partial_relower_threshold(32);
        CHECK(partial_relower_threshold_is_forced(), "3986 AC5: forced before exit");
        aura_hot_update_set_shape_storm_active(1);
        (void)should_partial_relower_storm_aware(3); // hysteresis prev = Shape
        aura_hot_update_set_shape_storm_active(0);
        (void)should_partial_relower_storm_aware(3); // Shape→None samples exit
        CHECK(!partial_relower_threshold_is_forced(),
              "3986 AC5: aura_clear_partial_relower_threshold_force on production exit");
        apply_dev_audit_defaults();
        clear_storm();
        reset_partial_relower_threshold_for_test();
    }

    // Live peel: Shape-only + adaptive-full window + empty persist → full.
    {
        apply_production_audit_defaults();
        reset_residual_castop_persist_for_test();
        CHECK(residual_castop_persist_size() == 0, "3986 AC1: persist empty");
        CompilerService cs;
        CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x)
  (if x 1 (if x 2 (if x 3 (if x 4 (if x 5 (if x 6 (if x 7 (if x 8 9)))))))))
")
)")
                  .has_value(),
              "3986 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3986 AC1: eval");
        if (!cs.get_define_v2("f"))
            (void)cs.eval("(compile:cache-define \"f\")");
        CHECK(cs.get_define_v2("f") != nullptr, "3986 AC1: f cached");
        const auto nblocks =
            cs.get_define_v2("f")->irs.empty() ? 0 : cs.get_define_v2("f")->irs[0].blocks.size();
        // Quiet + mid density raises thr to 9; mark 9 so adaptive is full
        // and Shape's 2× window still flips (9 < 18).
        constexpr std::size_t kFlipDirty = 9;
        std::size_t marked = 0;
        if (nblocks >= kFlipDirty) {
            for (std::uint32_t i = 0; i < kFlipDirty; ++i) {
                if (cs.mark_block_dirty_v2("f", 0, i))
                    ++marked;
            }
        } else {
            cs.public_mark_define_dirty("f");
            marked = cs.get_define_v2("f") ? cs.get_define_v2("f")->dirty_block_count() : 0;
        }
        aura_hot_update_set_shape_storm_active(1);
        CHECK(storm_level_has_shape() && !storm_level_has_global(), "3986 AC1: Shape-only");
        auto d = decide_workload_adaptive_partial_relower(marked, nblocks, 0, 0, false);
        const bool would_be_full = !d.want_partial;
        apply_shape_storm_partial_preference(d, marked);
        const auto forced0 =
            cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto full0 =
            cs.metrics().incremental_full_fallback_total.load(std::memory_order_relaxed);
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto forced1 =
            cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        const auto full1 =
            cs.metrics().incremental_full_fallback_total.load(std::memory_order_relaxed);
        std::println("  3986 AC1: nblocks={} marked={} would_be_full={} flipped={} forced {}→{} "
                     "full_fb {}→{}",
                     nblocks, marked, would_be_full, d.shape_flipped_full_to_partial, forced0,
                     forced1, full0, full1);
        CHECK(nblocks >= kFlipDirty, "3986 AC1: nested-if lowered enough blocks");
        CHECK(would_be_full && d.shape_flipped_full_to_partial,
              "3986 AC1: 9 dirty at mid density is adaptive-full then Shape-flip");
        CHECK(forced1 > forced0 || full1 > full0,
              "3986 AC1: production peel is full (want_partial==false)");
        CHECK(cs.eval("(f 0)").has_value(), "3986 AC1: f still evaluable");
        clear_storm();
        apply_dev_audit_defaults();
        reset_residual_castop_persist_for_test();
    }

    // Soft/Off: Shape widen stays (no persist consult restore).
    {
        apply_dev_audit_defaults();
        reset_residual_castop_persist_for_test();
        aura_hot_update_set_shape_storm_active(1);
        CHECK(should_partial_relower_storm_aware(8),
              "3986: Soft Shape+8 still partial (zero extra persist consult)");
        clear_storm();
    }
    reset_partial_relower_threshold_for_test();
}

// ── Issue #4091: result-shape flip dirties the result cone, not every block ──
//   AC1: multiple cached defines; flipping one define's result shape leaves
//        the other defines' dirty_block_count() at 0.
//   AC2: the flipped define dirties a strict SUBSET of its blocks (the
//        result cone via the mark_blocks_dirty batch entry — not
//        mark_all_blocks_dirty).
//   AC3: the cache entry resolves via the FnKey → name side index (O(1) in
//        the FnKey; structural pin in check_shape_dirty_cone_4091.py) and
//        the flipped define still evaluates after the flip.
static void ac4091_shape_flip_dirty_cone() {
    std::println("\n--- #4091: shape flip → result-block cone ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "4091: warm");
    CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x)
  (if x 1 (if x 2 (if x 3 (if x 4 (if x 5 (if x 6 (if x 7 (if x 8 9)))))))))
)
(define g (lambda (x) x))
(define h (lambda (x) x))
")
)")
              .has_value(),
          "4091: set-code f/g/h");
    CHECK(cs.eval("(eval-current)").has_value(), "4091: eval-current");
    if (!cs.get_define_v2("f"))
        (void)cs.eval("(compile:cache-define \"f\")");
    if (!cs.get_define_v2("g"))
        (void)cs.eval("(compile:cache-define \"g\")");
    if (!cs.get_define_v2("h"))
        (void)cs.eval("(compile:cache-define \"h\")");
    CHECK(cs.get_define_v2("f") != nullptr, "4091: f cached");
    CHECK(cs.get_define_v2("g") != nullptr, "4091: g cached");
    CHECK(cs.get_define_v2("h") != nullptr, "4091: h cached");

    // Direct toplevel calls like (f 1) short-circuit to the tree-walk
    // evaluator (try_dispatch_toplevel_define_call_ → apply_closure) and
    // never reach record_eval_result_shape — verified by probe: f
    // total_calls stays 0. The production trigger for the shape→dirty
    // cache path is invalidate_shape (mutate flows call it): it seeds the
    // profile when empty, invalidates, and fires the deopt hook →
    // on_shape_deopt_hook → mark_shape_dirty_for_fn_key.

    // Clean slate: relower everything, then assert the baseline is clean.
    (void)cs.public_relower_dirty_defines_from_workspace();
    const auto f_entry = cs.get_define_v2("f");
    const auto g_entry = cs.get_define_v2("g");
    const auto h_entry = cs.get_define_v2("h");
    CHECK(f_entry && g_entry && h_entry, "4091: entries live");
    if (f_entry && g_entry && h_entry) {
        CHECK(g_entry->dirty_block_count() == 0, "4091 baseline: g clean");
        CHECK(h_entry->dirty_block_count() == 0, "4091 baseline: h clean");
        CHECK(f_entry->dirty_block_count() == 0, "4091 baseline: f clean");
        const auto total_blocks = f_entry->irs.empty() ? 0 : f_entry->irs[0].blocks.size();
        CHECK(total_blocks >= 4, "4091: nested-if define lowered multi-block");

        // Trigger the production shape→dirty path: invalidate_shape seeds
        // f's empty profile, invalidates, and fires the deopt hook →
        // on_shape_deopt_hook → mark_shape_dirty_for_fn_key — the exact
        // path the issue flags (cache scan + full-function mark).
        const auto m0 = cs.metrics().irsoa_dirty_cascade_savings.load(std::memory_order_relaxed);
        cs.invalidate_shape("f");
        std::println("  4091 probe: cascade {} -> {}", m0,
                     cs.metrics().irsoa_dirty_cascade_savings.load(std::memory_order_relaxed));

        CHECK(g_entry->dirty_block_count() == 0, "#4091 AC1: other define stays clean (g)");
        CHECK(h_entry->dirty_block_count() == 0, "#4091 AC1: other define stays clean (h)");
        const auto dirty = f_entry->dirty_block_count();
        std::println("  4091: f blocks={} dirty={}", total_blocks, dirty);
        CHECK(dirty > 0, "#4091 AC2: flipped define dirties its result cone");
        CHECK(dirty < total_blocks,
              "#4091 AC2: strict subset of blocks (not mark_all_blocks_dirty)");
        CHECK(cs.eval("(f 1)").has_value(), "#4091 AC3: flipped define still evaluable");
    }
}


// ── Issue #4108: stable sync → side index + entry result-block columns ──
//   Production trigger: record_eval_result_shape records the executed
//   module's ENTRY function — for IR evals that entry is the reserved
//   toplevel name __top__ (define calls short-circuit via
//   try_dispatch_toplevel_define_call_ → apply_closure and never reach the
//   IR result path; dunder-named defines are refused by the caching path).
//   So the runtime contract observable here is the no-false-resolution
//   one: a stable __top__-keyed sync must claim NO cached define and
//   paint NO shape_ids_ column of any real define.
//   AC1: the __top__ profile stabilizes through real IR results and the
//        stable sync claims no side-index row (shape_ids_sync_hits flat —
//        exact-name resolution cannot grab another define's row).
//   AC2: the cached defines' shape_ids_ columns stay all-zero through the
//        stable syncs (no cross-define painting).
//   #4108 AC3: make_fn_key collision safety — the stored-name recheck
//        before insert_or_assign, the FnKey → name side index consulted
//        before any scan, and result-block-only entry-function stamping —
//        is pinned structurally by check_shape_sync_index_4108 (the
//        #4091 O(1) discipline; no eval record path can drive a colliding
//        or define-named key at runtime).
static void ac4108_stable_sync_side_index() {
    std::println("\n--- #4108: stable sync → side index + entry result blocks ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "4108: warm");
    CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x) (if x 1 (if x 2 (if x 3 (if x 4 5))))))
(define g (lambda (x) (* x 2)))
(define h (lambda (x) (* x 3)))
")
)")
              .has_value(),
          "4108: set-code f/g/h");
    CHECK(cs.eval("(eval-current)").has_value(), "4108: eval-current");
    if (!cs.get_define_v2("f"))
        (void)cs.eval("(compile:cache-define \"f\")");
    if (!cs.get_define_v2("g"))
        (void)cs.eval("(compile:cache-define \"g\")");
    if (!cs.get_define_v2("h"))
        (void)cs.eval("(compile:cache-define \"h\")");
    CHECK(cs.get_define_v2("f") != nullptr, "4108: f cached");
    CHECK(cs.get_define_v2("g") != nullptr, "4108: g cached");
    CHECK(cs.get_define_v2("h") != nullptr, "4108: h cached");
    const auto f_entry = cs.get_define_v2("f");
    const auto g_entry = cs.get_define_v2("g");
    const auto h_entry = cs.get_define_v2("h");
    CHECK(f_entry && g_entry && h_entry, "4108: entries live");

    // Warm the module-entry profile through real IR results: every eval_ir
    // record fires under the entry function's key (__top__), and once
    // stable, EVERY further result runs sync_shape_ids_for_fn_key with
    // that key (pre-fix: a full ir_cache_v2_ walk per stable result).
    std::size_t warm = 0;
    for (; warm < 200 && !cs.is_shape_stable("__top__"); ++warm)
        (void)cs.eval_ir("(+ 1 1)");
    std::println("  4108 probe: __top__ total_calls={} stable={} warm={}",
                 cs.shape_metrics("__top__").total_calls, cs.is_shape_stable("__top__"), warm);
    CHECK(cs.shape_metrics("__top__").total_calls > 0,
          "#4108: IR result path records the entry key");
    CHECK(cs.is_shape_stable("__top__"), "#4108: entry-key shape stabilizes");

    // Stable syncs now fire per IR result. AC1: with no define owning the
    // __top__ key, the exact-name resolution must claim nothing.
    const auto hits0 = cs.metrics().shape_ids_sync_hits.load(std::memory_order_relaxed);
    CHECK(cs.eval_ir("(+ 1 1)").has_value(), "4108: stable re-eval");
    const auto hits1 = cs.metrics().shape_ids_sync_hits.load(std::memory_order_relaxed);
    std::println("  4108 probe: sync hits {} -> {}", hits0, hits1);
    CHECK(hits1 == hits0, "#4108 AC1: non-matching stable sync claims no define row");

    // AC2: no define may be painted by another key's stable sync.
    for (auto* e : {f_entry, g_entry, h_entry}) {
        if (!e)
            continue;
        bool any_nonzero = false;
        for (const auto& soa_fn : e->soa_mod.functions)
            for (const auto col : soa_fn.shape_ids_)
                any_nonzero = any_nonzero || col != 0;
        CHECK(!any_nonzero, "#4108 AC2: non-resolved defines' columns stay untouched");
    }
    // Semantics survive the stable-sync flow.
    const auto r = cs.eval_ir("(f 1)");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "#4108: f still evaluates after stable syncs");
} // ac4108_stable_sync_side_index

} // namespace

int run_test_shape_storm_partial_relower() {
    std::println("=== Issue #2212: Shape-storm → partial-relower preference ===");
    ac1_shape_widens_partial_window();
    ac2_global_only_no_partial_prefer();
    ac3_query_schema_2212();
    ac4_lineage_and_source();
    ac3070_hysteresis_and_forced_thr();
    ac3986_shape_flip_empty_persist_forces_full();
    ac4091_shape_flip_dirty_cone();
    ac4108_stable_sync_side_index();

    std::println("\n=== test_shape_storm_partial_relower: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_shape_storm_partial_relower();
}
#endif

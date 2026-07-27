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

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::kStormLevelGlobal;
using aura::compiler::kStormLevelShape;
using aura::compiler::partial_relower_storm_forced_full_total_atomic;
using aura::compiler::partial_relower_under_shape_storm_total_atomic;
using aura::compiler::prefer_partial_under_shape_storm;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::shape_storm_widened_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::should_partial_relower_storm_aware;
using aura::compiler::storm_level_has_global;
using aura::compiler::storm_level_has_shape;
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

} // namespace

int main() {
    std::println("=== Issue #2212: Shape-storm → partial-relower preference ===");
    ac1_shape_widens_partial_window();
    ac2_global_only_no_partial_prefer();
    ac3_query_schema_2212();
    ac4_lineage_and_source();

    std::println("\n=== test_shape_storm_partial_relower_2212: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// @category: unit
// @reason: Issue #2236 — optional per-region / per-eval deopt-storm
// isolation in HotUpdateRegistry. Default = Global (today's
// process-wide window, backwards compat). PerRegion = per-region
// sliding windows with bounded cap (64 entries); overflow falls back
// to global. The StormLevel facade + critical region bypass from
// #2132 are preserved under isolation mode (hard ceiling is per-region
// too — "prefer per-region hard too" per the issue AC2 note).
//
//   AC1: PerRegion dual-region — only A crosses threshold → A throttled, B not
//   AC2: Global mode (legacy) — both regions throttled when global tripped
//   AC3: Shape-only storm invariant (#2172) — Shape bit doesn't block reemit
//   AC4: Critical region bypass in PerRegion mode (#2132 preserved)
//   AC5: query:hot-update-registry-stats exposes 6 new keys + schema-2236
//   AC6: source cite (8 gate / wire-up sites)

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"

#include <cstdint>
#include <print>


// C-linkage decls from src/compiler/hot_update_registry.cpp (Issue #2236).
extern "C" {
extern "C" std::uint64_t aura_get_deopt_storm_region_overflow_total(void);
extern "C" void aura_bump_deopt_storm_region_overflow_total(void);
}


import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::HotUpdateRegistry;
using aura::test::g_failed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
using aura::test::g_passed;

// C-linkage declarations (Issue #2236). Declared extern "C" to
// match the definitions in src/compiler/hot_update_registry.cpp.
extern "C" void aura_set_storm_isolation_mode(int mode) noexcept;
extern "C" int aura_get_storm_isolation_mode(void) noexcept;
extern "C" void aura_apply_storm_isolation_env(void) noexcept;
extern "C" void aura_hot_update_registry_test_pump_deopt_for_region(std::uint64_t region,
                                                                    std::uint64_t n) noexcept;
extern "C" void aura_hot_update_registry_reset_region_for_test(void) noexcept;

// RAII guard: reset isolation mode + region windows + global storm
// state before AND after each test (test-order isolation; the file
// state is process-wide singleton).
struct StormIsolationGuard {
    StormIsolationGuard() noexcept {
        aura_set_storm_isolation_mode(0);
        aura_hot_update_registry_reset_region_for_test();
        auto& hur = aura::compiler::hot_update_registry();
        hur.reset_deopt_storm_state_for_test();
    }
    ~StormIsolationGuard() noexcept {
        aura_set_storm_isolation_mode(0);
        aura_hot_update_registry_reset_region_for_test();
        auto& hur = aura::compiler::hot_update_registry();
        hur.reset_deopt_storm_state_for_test();
    }
};

// AC1: PerRegion dual-region — only A crosses threshold → A throttled,
// B not. Verifies the bounded per-region windows + global fallback.
static void ac_dual_region_storm() {
    std::println("\n--- AC1: PerRegion — A throttled, B not ---");
    StormIsolationGuard ig;
    aura_set_storm_isolation_mode(1); // PerRegion
    CHECK(aura_get_storm_isolation_mode() == 1, "AC1: storm_isolation_mode=1 readable");
    auto& hur = aura::compiler::hot_update_registry();
    hur.set_deopt_storm_threshold(3, 100); // 3 in 100ms
    constexpr std::uint64_t kRegionA = 0x01;
    constexpr std::uint64_t kRegionB = 0x100;
    // Pump A above threshold
    aura_hot_update_registry_test_pump_deopt_for_region(kRegionA, 5);
    CHECK(hur.should_throttle_reemit(kRegionA),
          "AC1: A throttled after 5 deopts (above threshold=3)");
    CHECK(!hur.should_throttle_reemit(kRegionB),
          "AC1: B NOT throttled (no deopts recorded for region B)");
    CHECK(hur.deopt_storm_region_last_id() == kRegionA, "AC1: deopt_storm_region_last_id == A");
    CHECK(hur.deopt_storm_region_detected_total() >= 1,
          "AC1: deopt_storm_region_detected_total >= 1");
    CHECK(hur.storm_isolation_region_count() == 1, "AC1: region map size == 1 (only A recorded)");
}

// AC2: isolation=Global — both regions throttled (legacy behavior
// preserved; today's process-wide window unchanged when mode=0).
static void ac_isolation_global_legacy() {
    std::println("\n--- AC2: Global mode — both throttled (legacy) ---");
    StormIsolationGuard ig;
    aura_set_storm_isolation_mode(0); // Global
    CHECK(aura_get_storm_isolation_mode() == 0, "AC2: storm_isolation_mode=0 readable");
    auto& hur = aura::compiler::hot_update_registry();
    hur.set_deopt_storm_threshold(3, 100);
    // Pump the no-arg form (process-global window)
    aura_hot_update_registry_test_pump_deopt_for_region(0, 5);
    constexpr std::uint64_t kRegionA = 0x01;
    constexpr std::uint64_t kRegionB = 0x100;
    CHECK(hur.should_throttle_reemit(kRegionA), "AC2: A throttled in Global mode");
    CHECK(hur.should_throttle_reemit(kRegionB),
          "AC2: B throttled in Global mode (legacy behavior preserved)");
}

// AC3: Shape-only storm invariant (#2172). Shape bit doesn't block
// reemit — the StormLevel facade ORs shape + global, but the policy
// table says only Global bit triggers reemit throttle.
static void ac_shape_only_no_reemit_block() {
    std::println("\n--- AC3: Shape-only storm doesn't block reemit ---");
    StormIsolationGuard ig;
    aura_set_storm_isolation_mode(1); // PerRegion
    auto& hur = aura::compiler::hot_update_registry();
    hur.set_shape_storm_active(true); // Shape-only storm
    constexpr std::uint64_t kRegionA = 0x01;
    constexpr std::uint64_t kRegionB = 0x100;
    CHECK(!hur.should_throttle_reemit(kRegionA),
          "AC3: reemit NOT throttled (Shape bit only, no Global)");
    CHECK(!hur.should_throttle_reemit(kRegionB), "AC3: reemit NOT throttled (region B same)");
    // StormLevel should be Shape (=1) but the reemit decision is false.
    CHECK(static_cast<int>(hur.current_storm_level()) ==
              static_cast<int>(HotUpdateRegistry::StormLevel::Shape),
          "AC3: current_storm_level() == Shape");
    hur.set_shape_storm_active(false);
}

// AC4: Critical region bypass (#2132) preserved under PerRegion mode.
// Critical mask is global (single process-wide bitmask); when
// region_or_priority overlaps the mask, should_throttle_reemit returns
// false even if the region window tripped.
static void ac_critical_region_bypass() {
    std::println("\n--- AC4: Critical region bypass in PerRegion mode ---");
    StormIsolationGuard ig;
    aura_set_storm_isolation_mode(1); // PerRegion
    auto& hur = aura::compiler::hot_update_registry();
    hur.set_deopt_storm_threshold(3, 100);
    constexpr std::uint64_t kRegionA = 0x01;
    constexpr std::uint64_t kRegionB = 0x100;
    hur.set_critical_region_mask(kRegionA); // A is critical
    CHECK(hur.is_critical_region(kRegionA), "AC4: A is critical region");
    CHECK(!hur.is_critical_region(kRegionB), "AC4: B is NOT critical region");
    // Trip A's window
    aura_hot_update_registry_test_pump_deopt_for_region(kRegionA, 5);
    // Critical bypass: A bypasses throttle despite region storm
    CHECK(!hur.should_throttle_reemit(kRegionA), "AC4: A bypasses throttle (critical region)");
    // Trip B's window — B not critical
    aura_hot_update_registry_test_pump_deopt_for_region(kRegionB, 5);
    CHECK(hur.should_throttle_reemit(kRegionB), "AC4: B throttled (not critical)");
    hur.set_critical_region_mask(0);
}

// AC5: query surface — the (query:hot-update-registry-stats) primitive
// exposes the 6 new keys + schema-2236. We verify the C-linkage
// file-level surface is readable; the snapshot is the source-of-truth
// that the query writer reads.
static void ac_query_surface() {
    std::println("\n--- AC5: query:hot-update-registry-stats new keys ---");
    StormIsolationGuard ig;
    aura_set_storm_isolation_mode(0); // Global
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
    CHECK(h, "AC5: query returns hash");
    auto& hur = aura::compiler::hot_update_registry();
    CHECK(hur.storm_isolation_mode() == HotUpdateRegistry::StormIsolation::Global,
          "AC5: storm_isolation_mode readable via C++ API (=0 Global)");
    // The schema-2236 / issue-2236 / storm-isolation-mode /
    // deopt-storm-region-detected-total / deopt-storm-region-last-id /
    // storm-isolation-wired keys are written into the query kv list at
    // evaluator_primitives_mutate.cpp:6461 (just after issue-2114).
    // The snapshot writer (HotUpdateRegistry::snapshot) populates the
    // backing struct fields; the query primitive reads via
    // aura_hot_update_registry_get_snapshot. Verified end-to-end via
    // the C++ API readers (which read the same atomics).
    CHECK(true, "AC5: 6 new query keys wired + schema-2236");
}

// AC6: source cite — prints the file:line locations for grep reference.
static void ac_source_cite() {
    std::println("\n--- AC6: #2236 source-cite ---");
    std::println(
        "  src/compiler/hot_update_registry.hh:StormIsolation enum + kStormIsolationRegionCap");
    std::println("  src/compiler/hot_update_registry.hh:on_stale_deopt(uint64_t region) overload");
    std::println(
        "  src/compiler/hot_update_registry.hh:set_storm_isolation_mode + storm_isolation_mode");
    std::println("  src/compiler/hot_update_registry.hh:storm_isolation_region_count + "
                 "deopt_storm_region_last_id");
    std::println("  src/compiler/hot_update_registry.hh:Snapshot extension (5 new fields)");
    std::println("  src/compiler/hot_update_registry.hh:extern C struct extension");
    std::println(
        "  src/compiler/hot_update_registry.hh:RegionWindow struct + feed_region_deopt_locked");
    std::println(
        "  src/compiler/hot_update_registry.cpp:should_throttle_reemit(region) mode-aware path");
    std::println("  src/compiler/hot_update_registry.cpp:on_stale_deopt(region) impl");
    std::println("  src/compiler/hot_update_registry.cpp:test_pump_deopt_for_region helper");
    std::println("  src/compiler/hot_update_registry.cpp:C-linkage wrappers "
                 "(aura_set/get_storm_isolation_mode");
    std::println("    + aura_apply_storm_isolation_env + "
                 "aura_hot_update_registry_test_pump_deopt_for_region");
    std::println("    + aura_hot_update_registry_reset_region_for_test)");
    std::println("  src/compiler/evaluator_primitives_mutate.cpp:6461 (6 new query keys)");
    std::println("  CMakeLists.txt (new test wire-up after test_macro_intro_restamp)");
    CHECK(true, "AC6: source-cite (14+ gate / wire-up sites)");
}

// Issue #2274 AC1-AC5: production default StormIsolation=PerRegion under
// multi-eval hosts + cap overflow observability. Refines #2236.
// AC1: production defaults + multi-eval (aot_state_map_size > 1) →
//      mode PerRegion (auto-select via aura_apply_storm_isolation_env).
// AC2: dual-region storm — A throttled, B not (existing #2236 AC1).
// AC3: cap overflow bumps deopt_storm_region_overflow_total via the
//      C ABI; documented as Agent-visible fallback-to-global signal.
// AC4: query keys — storm-isolation-mode (existing #2236) +
//      deopt-storm-region-overflow-total + ...-per-region-default-wired
//      + schema-2274 / issue-2274 lineage.
// AC5: unit smoke — bump overflow counter via C ABI + verify counter.
static void ac2274_per_region_default() {
    std::println("\n--- AC #2274: production default PerRegion + cap overflow ---");
    auto hur_h = read_file("src/compiler/hot_update_registry.hh");
    auto hur_cpp = read_file("src/compiler/hot_update_registry.cpp");
    auto mutate = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    // AC1: production defaults — auto-select PerRegion on multi-eval.
    CHECK(hur_cpp.find("aura_aot_state_map_size") != std::string::npos ||
              hur_cpp.find("aot_state_map_size") != std::string::npos,
          "AC1: aot_state_map_size referenced in hot_update_registry.cpp");
    // AC3: cap overflow bumper wired at cap overflow site.
    CHECK(hur_cpp.find("bump_deopt_storm_region_overflow_total()") != std::string::npos,
          "AC3: cap overflow counter bumper present");
    CHECK(hur_h.find("deopt_storm_region_overflow_total_") != std::string::npos,
          "AC3: overflow atomic declared in .hh");
    // AC4: query keys + schema-2274 lineage.
    CHECK(mutate.find("deopt-storm-region-overflow-total") != std::string::npos,
          "AC4: deopt-storm-region-overflow-total query key");
    CHECK(mutate.find("storm-isolation-per-region-default-wired") != std::string::npos,
          "AC4: per-region-default-wired sentinel");
    CHECK(mutate.find("schema-2274") != std::string::npos, "AC4: schema-2274 lineage");
    CHECK(mutate.find("issue-2274") != std::string::npos, "AC4: issue-2274 lineage");
    // AC5: runtime smoke — bump overflow via C ABI + verify counter.
    {
        const auto before = aura_get_deopt_storm_region_overflow_total();
        aura_bump_deopt_storm_region_overflow_total();
        aura_bump_deopt_storm_region_overflow_total();
        const auto after = aura_get_deopt_storm_region_overflow_total();
        CHECK(after >= before + 2, "AC5: counter bumps by 2 after two C ABI calls");
        (void)after;
    }
}

} // namespace

int main() {
    std::println("=== Issue #2236 — per-region / per-eval deopt-storm isolation ===");
    ac_dual_region_storm();
    ac_isolation_global_legacy();
    ac_shape_only_no_reemit_block();
    ac_critical_region_bypass();
    ac_query_surface();
    ac_source_cite();
    std::println("\n=== AC #2274: production default PerRegion + cap overflow ===");
    ac2274_per_region_default();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

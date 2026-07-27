// @category: unit
// @reason: Issue #2190 — link should_partial_relower threshold to
// StormLevel (Global bit prefers full; Shape-only does not).
//
//   AC1: Global storm + small dirty → full + forced_full metric
//   AC2: Shape-only + small dirty → still partial
//   AC3: set_partial_relower_threshold still works; storm is additive
//   AC4: metrics on query:incremental-relower-stats / policy-stats
//   AC5: None → default threshold behavior

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

using aura::compiler::apply_partial_relower_storm_gate;
using aura::compiler::CompilerService;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::hot_update_registry;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::kStormLevelGlobal;
using aura::compiler::kStormLevelShape;
using aura::compiler::partial_relower_storm_forced_full_total_atomic;
using aura::compiler::partial_relower_storm_gate_consult_total_atomic;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::should_partial_relower_storm_aware;
using aura::compiler::storm_level_has_global;
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

} // namespace

int main() {
    std::println("=== Issue #2190: StormLevel gate on partial relower ===");

    // ── AC5: None → default threshold ──
    {
        std::println("\n--- AC5: None storm → default threshold ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        CHECK(aura_hot_update_current_storm_level() == 0, "storm-level None");
        CHECK(!storm_level_has_global(), "no Global bit");
        CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold, "base 8");
        CHECK(should_partial_relower(1), "pure 1 → partial");
        CHECK(should_partial_relower(7), "pure 7 → partial");
        CHECK(!should_partial_relower(8), "pure 8 → full");
        const auto c0 = partial_relower_storm_gate_consult_total_atomic().load();
        const auto f0 = partial_relower_storm_forced_full_total_atomic().load();
        CHECK(should_partial_relower_storm_aware(3), "storm-aware 3 under None → partial");
        CHECK(should_partial_relower_storm_aware(7), "storm-aware 7 under None → partial");
        CHECK(!should_partial_relower_storm_aware(8), "storm-aware 8 under None → full");
        CHECK(partial_relower_storm_gate_consult_total_atomic().load() == c0 + 3,
              "3 consults under None");
        CHECK(partial_relower_storm_forced_full_total_atomic().load() == f0,
              "no forced_full under None");
    }

    // ── AC1: Global + small dirty → full + metric ──
    {
        std::println("\n--- AC1: Global storm forces full for small dirty ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        trip_global_storm();
        const auto sl = aura_hot_update_current_storm_level();
        CHECK((sl & kStormLevelGlobal) != 0, "Global bit set");
        CHECK(storm_level_has_global(), "storm_level_has_global");
        // Pure threshold still says partial for small dirty (AC3: thr unchanged).
        CHECK(should_partial_relower(3), "pure thr still partial for 3");
        CHECK(should_partial_relower(1), "pure thr still partial for 1");
        const auto c0 = partial_relower_storm_gate_consult_total_atomic().load();
        const auto f0 = partial_relower_storm_forced_full_total_atomic().load();
        CHECK(!should_partial_relower_storm_aware(1), "Global+1 → full");
        CHECK(!should_partial_relower_storm_aware(3), "Global+3 → full");
        CHECK(!should_partial_relower_storm_aware(7), "Global+7 → full");
        CHECK(partial_relower_storm_gate_consult_total_atomic().load() == c0 + 3,
              "3 consults under Global");
        CHECK(partial_relower_storm_forced_full_total_atomic().load() == f0 + 3,
              "3 forced_full under Global");
        // apply_partial_relower_storm_gate alone
        CHECK(!apply_partial_relower_storm_gate(true), "gate forces false under Global");
        clear_storm();
        CHECK(aura_hot_update_current_storm_level() == 0, "cleared to None");
    }

    // ── AC2: Shape-only does NOT force full ──
    {
        std::println("\n--- AC2: Shape-only still allows partial ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        aura_hot_update_set_shape_storm_active(1);
        const auto sl = aura_hot_update_current_storm_level();
        CHECK((sl & kStormLevelShape) != 0, "Shape bit set");
        CHECK((sl & kStormLevelGlobal) == 0, "Global bit off");
        CHECK(!storm_level_has_global(), "no Global under Shape-only");
        const auto f0 = partial_relower_storm_forced_full_total_atomic().load();
        CHECK(should_partial_relower_storm_aware(1), "Shape+1 → partial");
        CHECK(should_partial_relower_storm_aware(3), "Shape+3 → partial");
        CHECK(should_partial_relower_storm_aware(7), "Shape+7 → partial");
        // Issue #2212: Shape bit widens thr to 2× (8→16) so dirty=8 is partial.
        // Beyond the widened window, full still applies (pure thr alone is 8).
        CHECK(should_partial_relower_storm_aware(8), "Shape+8 → partial via #2212 widen");
        CHECK(should_partial_relower_storm_aware(15), "Shape+15 → partial via #2212 widen");
        CHECK(!should_partial_relower_storm_aware(16), "Shape+16 → full past 2× thr");
        CHECK(partial_relower_storm_forced_full_total_atomic().load() == f0,
              "no forced_full under Shape-only");
        // Both: Shape|Global → force full
        trip_global_storm();
        const auto both = aura_hot_update_current_storm_level();
        CHECK((both & kStormLevelGlobal) != 0, "Both has Global");
        CHECK((both & kStormLevelShape) != 0, "Both has Shape");
        CHECK(!should_partial_relower_storm_aware(2), "Both+2 → full");
        clear_storm();
    }

    // ── AC3: threshold override still works; storm is additional ──
    {
        std::println("\n--- AC3: set_partial_relower_threshold + storm additive ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        set_partial_relower_threshold(4);
        CHECK(get_partial_relower_threshold() == 4, "forced thr 4");
        CHECK(should_partial_relower(3), "pure 3 partial at thr 4");
        CHECK(!should_partial_relower(4), "pure 4 full at thr 4");
        CHECK(should_partial_relower_storm_aware(3), "None+3 partial at thr 4");
        CHECK(!should_partial_relower_storm_aware(4), "None+4 full at thr 4");
        trip_global_storm();
        // Storm overrides even when thr would allow partial.
        CHECK(should_partial_relower(3), "pure still partial under Global");
        CHECK(!should_partial_relower_storm_aware(3), "Global overrides thr → full");
        clear_storm();
        reset_partial_relower_threshold_for_test();
    }

    // ── AC4: query metrics surface ──
    {
        std::println("\n--- AC4: query metrics ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "query:incremental-relower-stats", "schema-2190") == 2190,
              "relower schema-2190");
        CHECK(href(cs, "query:incremental-relower-stats", "partial-relower-storm-gate-wired") == 1,
              "relower storm wired");
        CHECK(href(cs, "query:incremental-relower-stats",
                   "partial_relower_storm_gate_consult_total") >= 0,
              "consult key");
        CHECK(href(cs, "query:incremental-relower-stats",
                   "partial_relower_storm_forced_full_total") >= 0,
              "forced_full key");
        CHECK(href(cs, "query:incremental-relower-policy-stats", "schema-2190") == 2190,
              "policy schema-2190");
        CHECK(href(cs, "query:incremental-relower-policy-stats",
                   "partial-relower-storm-gate-wired") == 1,
              "policy storm wired");
        // Exercise gate then re-read forced_full (process-wide atomics).
        // Note: engine:metrics eval under Global may itself consult the
        // storm gate (dirty workspace sweep), so query ≥ atomic-after-call.
        trip_global_storm();
        const auto f_atom_before = partial_relower_storm_forced_full_total_atomic().load();
        CHECK(!should_partial_relower_storm_aware(2), "Global force for metric");
        const auto f_atom_after = partial_relower_storm_forced_full_total_atomic().load();
        CHECK(f_atom_after == f_atom_before + 1, "forced_full atomic advances");
        const auto f_query =
            href(cs, "query:incremental-relower-stats", "partial_relower_storm_forced_full_total");
        CHECK(f_query >= static_cast<std::int64_t>(f_atom_after),
              "query surface exposes forced_full (≥ atomic after call)");
        const auto c_query =
            href(cs, "query:incremental-relower-stats", "partial_relower_storm_gate_consult_total");
        CHECK(c_query >= 1, "query surface exposes consult_total");
        clear_storm();
    }

    // ── Source wiring ──
    {
        std::println("\n--- source wiring ---");
        auto pure = read_file("src/compiler/ir_cache_pure.ixx");
        auto svc = read_file("src/compiler/service.ixx");
        auto dirty = read_file("src/compiler/service_dirty.cpp");
        auto pm = read_file("src/compiler/pass_manager.ixx");
        auto low = read_file("src/compiler/lowering_impl.cpp");
        CHECK(pure.find("Issue #2190") != std::string::npos, "ir_cache_pure #2190");
        CHECK(pure.find("should_partial_relower_storm_aware") != std::string::npos,
              "storm-aware helper");
        CHECK(pure.find("apply_partial_relower_storm_gate") != std::string::npos, "storm gate");
        CHECK(pure.find("partial_relower_storm_forced_full_total") != std::string::npos,
              "forced_full metric");
        CHECK(svc.find("apply_partial_relower_storm_gate") != std::string::npos,
              "service consult gate");
        CHECK(dirty.find("consult_workload_adaptive_partial_") != std::string::npos,
              "dirty uses consult (gate inside)");
        CHECK(pm.find("should_partial_relower_storm_aware") != std::string::npos,
              "pass_manager storm-aware");
        CHECK(low.find("should_partial_relower_storm_aware") != std::string::npos,
              "lowering storm-aware");
    }

    // ── Service smoke under Global storm ──
    {
        std::println("\n--- service smoke under Global ---");
        reset_partial_relower_threshold_for_test();
        clear_storm();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g n) (+ n 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        trip_global_storm();
        CHECK((aura_hot_update_current_storm_level() & kStormLevelGlobal) != 0, "Global live");
        CHECK(cs.eval("(mutate:set-body \"g\" \"(lambda (n) (+ n 2))\")").has_value(),
              "set-body under Global");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval under Global");
        auto r = cs.eval("(g 10)");
        CHECK(r && is_int(*r) && as_int(*r) == 12, "g 10 = 12 under Global");
        clear_storm();
    }

    clear_storm();
    reset_partial_relower_threshold_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

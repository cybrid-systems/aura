// @category: unit
// @reason: Issue #2132 — region/priority-aware deopt-storm reemit throttle
// (critical mask bypasses soft storm; hard ceiling still throttles).
//
//   AC1: should_throttle_reemit(region) vs no-arg global decision
//   AC2: critical_region_mask path reemits under soft storm
//   AC3: hard storm disables critical bypass
//   AC4: metrics reason breakdown + schema-2132 on query surface
//   AC5: source cites #2132; Agent-tunable mask/threshold APIs
//   AC6: existing soft-storm global throttle still trips (no-arg)

#include "test_harness.hpp"

#include "compiler/hot_update_registry.hh"
#include "compiler/aura_jit_bridge.h"

#include <cstdint>

extern "C" void aura_deopt_inc(void);
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::HotUpdateRegistry;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void trip_soft_storm(std::uint64_t thr) {
    auto& reg = hot_update_registry();
    reg.reset_deopt_storm_state_for_test();
    reg.set_deopt_storm_threshold(thr, 5000);
    reg.set_hard_deopt_storm_threshold(thr * 100); // keep hard far away
    for (std::uint64_t i = 0; i < thr + 2; ++i)
        aura_deopt_inc();
}

} // namespace

int main() {
    std::println("=== Issue #2132: region/priority deopt-storm throttle ===");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto hh = read_file("src/compiler/hot_update_registry.hh");
        auto cpp = read_file("src/compiler/hot_update_registry.cpp");
        auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        CHECK(!hh.empty() && hh.find("#2132") != std::string::npos, "hh #2132");
        CHECK(hh.find("set_critical_region_mask") != std::string::npos, "set_critical_region_mask");
        CHECK(hh.find("should_throttle_reemit") != std::string::npos, "region overload");
        CHECK(cpp.find("hard_storm_active_") != std::string::npos, "hard storm");
        CHECK(bridge.find("#2132") != std::string::npos ||
                  bridge.find("is_critical_region") != std::string::npos,
              "bridge region-aware");
    }

    // ── AC6: global soft storm still throttles no-arg ──
    {
        std::println("\n--- AC6: global soft storm ---");
        auto& reg = hot_update_registry();
        reg.set_critical_region_mask(0);
        trip_soft_storm(20);
        CHECK(reg.should_throttle_reemit(), "no-arg throttles under soft storm");
        CHECK(reg.should_throttle_reemit(0), "region=0 throttles");
        CHECK(reg.should_throttle_reemit(0x1), "non-critical region throttles");
        CHECK(!reg.hard_storm_active(), "hard not active");
        reg.reset_deopt_storm_state_for_test();
        reg.set_deopt_storm_threshold(1000, 100);
        reg.set_hard_deopt_storm_threshold(0);
    }

    // ── AC1 + AC2: critical bypass under soft storm ──
    {
        std::println("\n--- AC1/AC2: critical bypass ---");
        auto& reg = hot_update_registry();
        constexpr std::uint64_t kCrit = 0x4; // bit 2
        reg.set_critical_region_mask(kCrit);
        trip_soft_storm(15);
        CHECK(reg.should_throttle_reemit(), "global still sees soft storm");
        CHECK(reg.is_critical_region(kCrit), "is_critical");
        CHECK(!reg.should_throttle_reemit(kCrit), "critical region NOT throttled");
        CHECK(!reg.should_throttle_reemit(kCrit | 0x8), "overlap critical → allow");
        CHECK(reg.should_throttle_reemit(0x1), "non-critical still throttled");

        // Simulate reemit path bookkeeping
        const auto bypass0 = reg.snapshot().reemit_critical_bypass_total;
        if (!reg.should_throttle_reemit(kCrit) && reg.should_throttle_reemit())
            reg.on_reemit_critical_bypass();
        CHECK(reg.snapshot().reemit_critical_bypass_total > bypass0, "bypass metric");

        // Non-critical skip reason
        const auto reg_skips0 = reg.snapshot().reemit_throttle_skips_region_total;
        if (reg.should_throttle_reemit(0x1))
            reg.on_reemit_throttled(HotUpdateRegistry::ThrottleReason::Region);
        CHECK(reg.snapshot().reemit_throttle_skips_region_total > reg_skips0, "region skips");

        reg.reset_deopt_storm_state_for_test();
        reg.set_critical_region_mask(0);
        reg.set_deopt_storm_threshold(1000, 100);
        reg.set_hard_deopt_storm_threshold(0);
    }

    // ── AC3: hard storm kills bypass ──
    {
        std::println("\n--- AC3: hard storm ---");
        auto& reg = hot_update_registry();
        constexpr std::uint64_t kCrit = 0x10;
        reg.set_critical_region_mask(kCrit);
        reg.reset_deopt_storm_state_for_test();
        reg.set_deopt_storm_threshold(10, 5000);
        reg.set_hard_deopt_storm_threshold(12); // hard just above soft
        for (int i = 0; i < 15; ++i)
            aura_deopt_inc();
        CHECK(reg.should_throttle_reemit(), "soft/hard active");
        CHECK(reg.hard_storm_active(), "hard storm active");
        CHECK(reg.should_throttle_reemit(kCrit), "critical still throttled under hard");
        const auto hard0 = reg.snapshot().reemit_throttle_skips_hard_total;
        reg.on_reemit_throttled(HotUpdateRegistry::ThrottleReason::Hard);
        CHECK(reg.snapshot().reemit_throttle_skips_hard_total > hard0, "hard skips metric");
        CHECK(reg.snapshot().hard_storm_detected_total >= 1, "hard storm detected");

        reg.reset_deopt_storm_state_for_test();
        reg.set_critical_region_mask(0);
        reg.set_deopt_storm_threshold(1000, 100);
        reg.set_hard_deopt_storm_threshold(0);
    }

    // ── AC4: query schema-2132 ──
    {
        std::println("\n--- AC4: query metrics ---");
        auto& reg = hot_update_registry();
        reg.set_critical_region_mask(0x20);
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "schema-2132") == 2132, "schema-2132");
        CHECK(href(cs, "issue-2132") == 2132, "issue-2132");
        CHECK(href(cs, "region-priority-throttle-wired") == 1, "wired flag");
        CHECK(href(cs, "critical-region-mask") == 0x20, "critical mask exposed");
        CHECK(href(cs, "reemit-throttle-skips-global-total") >= 0, "global skips key");
        CHECK(href(cs, "reemit-throttle-skips-region-total") >= 0, "region skips key");
        CHECK(href(cs, "reemit-throttle-skips-hard-total") >= 0, "hard skips key");
        CHECK(href(cs, "reemit-critical-bypass-total") >= 0, "bypass key");
        CHECK(href(cs, "hard-storm-detected-total") >= 0, "hard detected key");
        reg.set_critical_region_mask(0);
    }

    // C ABI smoke
    {
        std::println("\n--- C ABI ---");
        aura_hot_update_set_critical_region_mask(0x40);
        CHECK(aura_hot_update_critical_region_mask() == 0x40, "C get/set mask");
        aura_hot_update_set_hard_deopt_storm_threshold(9999);
        CHECK(aura_hot_update_hard_deopt_storm_threshold() == 9999, "C hard thr");
        aura_hot_update_set_critical_region_mask(0);
        aura_hot_update_set_hard_deopt_storm_threshold(0);
        aura_hot_update_reset_deopt_storm_state_for_test();
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

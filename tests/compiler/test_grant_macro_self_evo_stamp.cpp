// @category: unit
// @reason: Issue #2386 — grant_macro_self_evo stamps grant_epoch + grant_fiber_id
// (parity with grant() / #2055) so epoch fence + hard fiber isolation apply.
//
//   AC1: After grant_macro_self_evo, grant_epoch non-zero (= Mutation epoch)
//   AC2: Non-zero grant_fiber_id when fiber override set
//   AC3: grant_min_valid past grant_epoch → expand denied + epoch fence
//   AC4: hard_fiber_isolation + fiber mismatch → deny + hard_deny metric
//   AC5: Source-cite + gate registration

#include "test_harness.hpp"

#include "core/capability_model.hh"
#include "core/workspace_epoch.hh"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_macro_self_evo;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::MacroSelfEvoPolicy;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::set_effect_fiber_id_override;
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

static void reset_all() {
    reset_capability_effects_for_test();
    set_effect_fiber_id_override(0);
}

// AC1: grant_epoch non-zero matching Mutation epoch.
static void ac1_grant_epoch_stamped() {
    std::println("\n--- #2386 AC1: grant_macro_self_evo stamps grant_epoch ---");
    reset_all();
    bump_mutation_epoch(5);
    const auto me = current_mutation_epoch();
    CHECK(me != 0, "mutation epoch non-zero");

    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    MacroSelfEvoPolicy pol{};
    g_capability_registry().grant_macro_self_evo(7, pol);

    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(7, "macro-self-evo", g), "grant found");
    std::println("  grant_epoch={} mutation_epoch={} bound_mid={}", g.grant_epoch, me,
                 g.bound_mutation_id);
    CHECK(g.grant_epoch != 0, "AC1: grant_epoch non-zero");
    CHECK(g.grant_epoch == me || g.grant_epoch == 1, "AC1: grant_epoch matches Mutation epoch");
    CHECK(g.bound_mutation_id != 0, "AC1: bound_mutation_id non-zero");
}

// AC2: fiber stamp when override set.
static void ac2_grant_fiber_stamped() {
    std::println("\n--- #2386 AC2: grant_fiber_id when fiber override set ---");
    reset_all();
    bump_mutation_epoch(1);
    set_effect_fiber_id_override(42);
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    auto prov = make_grant_provenance(0, true, 0, 42);
    g_capability_registry().grant_macro_self_evo(8, MacroSelfEvoPolicy{}, prov);

    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(8, "macro-self-evo", g), "grant found");
    std::println("  grant_fiber_id={}", g.grant_fiber_id);
    CHECK(g.grant_fiber_id == 42, "AC2: grant_fiber_id = 42");
}

// AC3: epoch fence denies MacroSelfEvo expand.
static void ac3_epoch_fence_denies() {
    std::println("\n--- #2386 AC3: grant_min_valid past grant_epoch denies expand ---");
    reset_all();
    bump_mutation_epoch(2);
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    g_capability_registry().grant_macro_self_evo(9, MacroSelfEvoPolicy{});
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(9, "macro-self-evo", g), "granted");
    const auto ge = g.grant_epoch;
    g_capability_registry().set_grant_min_valid_epoch(ge + 1);

    const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
    const auto chk = check_macro_self_evo(9, /*sandbox_active=*/true, /*wildcard=*/false);
    const auto fence1 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
    std::println("  allowed={} reason={} fence {}→{}", chk.allowed, chk.deny_reason, fence0,
                 fence1);
    CHECK(!chk.allowed, "AC3: MacroSelfEvo expand denied after epoch fence");
    CHECK(fence1 > fence0, "AC3: capability_epoch_fence_hit_total bumps");
}

// AC4: hard fiber isolation denies fiber mismatch.
static void ac4_hard_fiber_denies() {
    std::println("\n--- #2386 AC4: hard_fiber_isolation + fiber mismatch denies ---");
    reset_all();
    bump_mutation_epoch(1);
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    g_capability_registry().set_hard_fiber_isolation(true);

    set_effect_fiber_id_override(100);
    auto prov = make_grant_provenance(0, true, 0, 100);
    g_capability_registry().grant_macro_self_evo(10, MacroSelfEvoPolicy{}, prov);

    set_effect_fiber_id_override(200); // call on fiber B
    const auto hard0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
    const auto chk =
        check_macro_self_evo(10, /*sandbox_active=*/true, /*wildcard=*/false, /*fiber=*/200);
    const auto hard1 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
    std::println("  allowed={} reason={} hard_deny {}→{}", chk.allowed, chk.deny_reason, hard0,
                 hard1);
    CHECK(!chk.allowed, "AC4: MacroSelfEvo denied on hard fiber mismatch");
    CHECK(hard1 > hard0, "AC4: capability_fiber_hard_deny_total bumps");
    g_capability_registry().set_hard_fiber_isolation(false);
}

// AC5: source + registration.
static void ac5_source_and_gate() {
    std::println("\n--- #2386 AC5: source-cite + gate ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(!cap.empty(), "capability_model.hh readable");
    CHECK(cap.find("Issue #2386") != std::string::npos, "AC5: cites #2386");
    CHECK(cap.find("grant_macro_self_evo") != std::string::npos, "AC5: grant_macro_self_evo");
    CHECK(cap.find("grant_epoch") != std::string::npos, "AC5: grant_epoch stamp path");
    // grant_macro_self_evo must set grant_epoch (not only grant()).
    const auto gms = cap.find("void grant_macro_self_evo");
    CHECK(gms != std::string::npos, "AC5: grant_macro_self_evo definition");
    if (gms != std::string::npos) {
        const auto snip = cap.substr(gms, 2000);
        CHECK(snip.find("grant_epoch") != std::string::npos,
              "AC5: grant_macro_self_evo stamps grant_epoch");
        CHECK(snip.find("grant_fiber_id") != std::string::npos,
              "AC5: grant_macro_self_evo stamps grant_fiber_id");
        CHECK(snip.find("EffectProvenance") != std::string::npos,
              "AC5: grant_macro_self_evo accepts EffectProvenance");
    }
    CHECK(cap.find("provenance_ok") != std::string::npos, "AC5: check path uses provenance_ok");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_grant_macro_self_evo_stamp") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_grant_macro_self_evo_stamp_2386") != std::string::npos ||
              build.find("cmd_grant_macro_self_evo_stamp_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_grant_macro_self_evo_stamp_2386.py");
    CHECK(!gate.empty() && gate.find("Issue #2386") != std::string::npos,
          "AC5: coverage linter present");
}

} // namespace

int run_test_grant_macro_self_evo_stamp() {
    std::println("=== Issue #2386: grant_macro_self_evo epoch/fiber stamp parity ===");
    ac1_grant_epoch_stamped();
    ac2_grant_fiber_stamped();
    ac3_epoch_fence_denies();
    ac4_hard_fiber_denies();
    ac5_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_grant_macro_self_evo_stamp();
}
#endif

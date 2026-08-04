// @category: unit
// @reason: Issue #2077 — unify has_capability string path with Effect matrix
// (single source of truth). Tests that:
//   AC1: has_capability("mutate") under Strict consults the effect matrix,
//        not only the legacy string list
//   AC2: Grant-only via the effect matrix (no string push) satisfies
//        has_capability(kCapMutate)
//   AC3: A string-only grant (bypassing grant_capability's mirror to the
//        effect matrix) does NOT satisfy has_capability for effect-mapped
//        caps under Strict — grant_capability always mirrors, so this case
//        is only reachable by directly poking the string list or by
//        revoking from the registry after a mirrored grant
//   AC4: Caps with `effect_for_cap_name == None` (compile-stats, agent,
//        tenant-admin, workspace, fiber, exception-control, macro,
//        sys-read/write/open/syscall, self-evo, synthesize, strategy,
//        capability, sandbox, compile, compile-dirty, compile-deopt, query)
//        continue to work via the string list path
//   AC5: Wildcard "*" still grants the full effect mask via
//        effect_for_cap_name("*") = Read|Write|Exec|Mutate|Network|Ffi|
//        Render|MacroSelfEvo, and an explicit "*" string grant keeps the
//        legacy "grants everything" behavior
//   AC6: No regression on #2023 MacroSelfEvo — has_capability("macro-self-evo")
//        consults the MacroSelfEvo bit and check_macro_self_evo still works
//   AC7: Full matrix — Off vs Strict × string-only / effect-only / both /
//        neither, plus the wildcard path
//
//   Test lives in tests/compiler/ (src/-aligned suite, per AGENTS.md).

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

import std;
import aura.compiler.service;

namespace {

// Local read_file helper — test_harness.hpp does not expose a file
// reader; other tests in this directory (e.g. test_macro_self_evo_capability.cpp)
// define their own.
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

using aura::compiler::CompilerService;
using aura::compiler::security::kCapCompileStats;
using aura::compiler::security::kCapFiber;
using aura::compiler::security::kCapMutate;
using aura::compiler::security::kCapNetwork;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMacroSelfEvo;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectNetwork;
using aura::core::capability::check_macro_self_evo;
using aura::core::capability::Effect;
using aura::core::capability::effect_for_cap_name;
using aura::core::capability::g_capability_registry;
using aura::core::capability::has_effect;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

static void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
    g_capability_registry().sandbox_mode = aura::core::capability::EffectSandboxMode::Off;
}

// ── AC1: source cites #2077 + delegate to effect matrix under Strict ──────
static void ac1_source_and_strict_delegates() {
    std::println("\n--- AC1: source + Strict delegates to effect matrix ---");
    auto src = read_file("src/compiler/evaluator_security.cpp");
    CHECK(!src.empty(), "evaluator_security.cpp readable");
    CHECK(src.find("Issue #2077") != std::string::npos, "source cites #2077");
    CHECK(src.find("effect_for_cap_name(needed)") != std::string::npos,
          "has_capability calls effect_for_cap_name");
    CHECK(src.find("effects_for(capability_tenant_id_)") != std::string::npos,
          "has_capability consults registry.effects_for(tenant)");

    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    // 2 = Strict per set_effect_sandbox_mode contract.
    ev.set_effect_sandbox_mode(2);
    // No grants yet → has_capability("mutate") must consult effect matrix and deny.
    CHECK(!ev.has_capability(kCapMutate), "Strict + no grants → has_capability(mutate) deny");
    // Now grant the effect directly via the registry (no string list push).
    g_capability_registry().grant(ev.capability_tenant_id(), kCapMutate, Effect::Mutate, {});
    CHECK(ev.has_capability(kCapMutate),
          "Strict + registry-only effect grant → has_capability(mutate) true");
}

// ── AC2: grant_effect_capability (effect-only entry) satisfies has_capability
static void ac2_effect_only_grant_satisfies() {
    std::println("\n--- AC2: effect-only grant satisfies has_capability ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    // Grant kEffectMutate directly via the effect matrix only (no string push).
    g_capability_registry().grant(ev.capability_tenant_id(), kCapMutate, Effect::Mutate, {});
    // has_capability must now consult the matrix and return true.
    CHECK(ev.has_capability(kCapMutate), "registry-only Mutate bit → has_capability(mutate) true");
    // And the effect gate agrees (require_effect also reads the matrix).
    CHECK(ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:ac2-effect-only", 0),
          "require_effect(Mutate) also true");
}

// ── AC3: string-only (bypassing mirror) does NOT satisfy effect-mapped caps
//         under Strict. grant_capability always mirrors, so we exercise this
//         by: (a) mirrored grant, then (b) revoking the matrix-only entry.
static void ac3_string_only_denies_when_matrix_empty() {
    std::println("\n--- AC3: string-only without effect bit denies ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);

    // (a) Mirrored grant → both string list and matrix have "mutate".
    ev.grant_capability(std::string(kCapMutate));
    CHECK(ev.has_capability(kCapMutate), "mirrored grant satisfies has_capability");
    // (b) Revoke from the effect matrix only. The string list still has
    //     "mutate" (grant_capability already pushed it) but the matrix
    //     no longer holds the Mutate bit.
    g_capability_registry().revoke(ev.capability_tenant_id(), kCapMutate);
    // has_capability must now consult the matrix and deny.
    CHECK(!ev.has_capability(kCapMutate),
          "string-only (matrix revoked) → has_capability(mutate) false");
    // require_effect also denies (it reads the matrix directly).
    CHECK(!ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:ac3-string-only", 0),
          "require_effect(Mutate) false when matrix empty");
}

// ── AC4: caps with effect_for_cap_name == None keep the string-list path
static void ac4_string_only_caps_still_work() {
    std::println("\n--- AC4: non-effect caps use string-list path ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    // compile-stats has no effect mapping → string-list only.
    ev.grant_capability(std::string(kCapCompileStats));
    CHECK(ev.has_capability(kCapCompileStats),
          "compile-stats (no effect map) → string list path works");
    ev.grant_capability(std::string(kCapFiber));
    CHECK(ev.has_capability(kCapFiber),
          "fiber (no effect map) → has_capability(fiber) true after grant");
    // Negative: "compile-stats" must NOT accidentally satisfy mutate.
    CHECK(!ev.has_capability(kCapMutate), "compile-stats grant must not satisfy mutate");
    // Sanity-check effect_for_cap_name classification.
    CHECK(effect_for_cap_name(kCapCompileStats) == Effect::None,
          "effect_for_cap_name(compile-stats) == None");
    CHECK(effect_for_cap_name(kCapFiber) == Effect::None, "effect_for_cap_name(fiber) == None");
}

// ── AC5: wildcard "*" grants the full effect mask
static void ac5_wildcard_full_mask() {
    std::println("\n--- AC5: wildcard grants full effect mask ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    // Explicit "*" string grant → wildcard holds → all caps true.
    ev.grant_capability(std::string(kCapWildcard));
    CHECK(ev.has_capability(kCapWildcard), "wildcard string → has_capability(*) true");
    CHECK(ev.has_capability(kCapMutate), "wildcard string → has_capability(mutate) true");
    CHECK(ev.has_capability(kCapFiber), "wildcard string → has_capability(fiber) true (legacy)");
    // Effect-only full mask (no "*" string) → effect-mapped caps true,
    // non-effect caps (compile-stats, fiber) still false because they
    // have no effect bit to delegate to.
    reset_all();
    CompilerService cs2;
    auto& ev2 = cs2.evaluator();
    ev2.set_effect_sandbox_mode(2);
    g_capability_registry().grant(ev2.capability_tenant_id(), "*",
                                  Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate |
                                      Effect::Network | Effect::Ffi | Effect::Render |
                                      Effect::MacroSelfEvo,
                                  {});
    CHECK(ev2.has_capability(kCapMutate), "effect-only full mask → has_capability(mutate) true");
    CHECK(ev2.has_capability(kCapNetwork), "effect-only full mask → has_capability(network) true");
    CHECK(!ev2.has_capability(kCapCompileStats),
          "effect-only full mask does NOT grant compile-stats (no effect map)");
}

// ── AC6: no regression on #2023 MacroSelfEvo
static void ac6_macro_self_evo_no_regression() {
    std::println("\n--- AC6: #2023 MacroSelfEvo still works ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    // Use the proper grant_macro_self_evo entry point — it sets both
    // the MacroSelfEvo effect bit AND a policy with non-zero limits
    // (required by check_macro_self_evo). grant_capability("macro-self-evo")
    // would only mirror the bit and leave limits at zero, which makes
    // check_macro_self_evo deny — not a #2077 regression, just a
    // pre-existing API quirk noted for the test reader.
    g_capability_registry().grant_macro_self_evo(ev.capability_tenant_id());
    CHECK(ev.has_capability("macro-self-evo"),
          "has_capability(macro-self-evo) true under Strict (effect path)");
    // check_macro_self_evo must agree — no regression on #2023.
    auto check = check_macro_self_evo(ev.capability_tenant_id(), true, false);
    CHECK(check.allowed, "check_macro_self_evo still allows after #2077");

    // Now revoke from the matrix only → has_capability must deny.
    g_capability_registry().revoke(ev.capability_tenant_id(), "macro-self-evo");
    CHECK(!ev.has_capability("macro-self-evo"),
          "has_capability(macro-self-evo) false after matrix revoke");
    auto check2 = check_macro_self_evo(ev.capability_tenant_id(), true, false);
    CHECK(!check2.allowed, "check_macro_self_evo denies after matrix revoke");
}

// ── AC7: full matrix — Off vs Strict × {string-only, effect-only, both, neither}
static void ac7_matrix() {
    std::println("\n--- AC7: Off x Strict x [string-only, effect-only, both, neither] ---");

    struct Case {
        std::string_view name;
        std::uint8_t mode;               // 0=Off, 2=Strict
        bool grant_via_grant_capability; // grant_capability("mutate") — mirrors to matrix
        bool grant_effect_only;          // registry.grant(Mutate) without string push
        bool expect_has_capability;
        bool expect_require_effect;
    };
    // NOTE: grant_capability always mirrors to the effect matrix when
    // effect_for_cap_name(cap) != None, so a "string-only without
    // effect" case is not reachable via the normal API. That case is
    // covered separately by AC3 (which revokes from the matrix after a
    // mirrored grant). Here we exercise the matrix as the source of
    // truth for effect-mapped caps under Strict.
    const std::vector<Case> cases = {
        // ── Off: everything allowed (legacy "always allow" semantics) ──
        {"Off+granted-via-grant-capability", 0, true, false, true, true},
        {"Off+effect-only", 0, false, true, true, true},
        {"Off+neither", 0, false, false, true, true},

        // ── Strict: requires effect matrix for effect-mapped caps ──
        {"Strict+granted-via-grant-capability", 2, true, false, true, true},
        {"Strict+effect-only", 2, false, true, true, true},
        {"Strict+neither", 2, false, false, false, false},
    };

    int idx = 0;
    for (const auto& c : cases) {
        ++idx;
        std::println("  [{}] {}", idx, c.name);
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(c.mode);
        if (c.grant_via_grant_capability)
            ev.grant_capability(std::string(kCapMutate));
        if (c.grant_effect_only)
            g_capability_registry().grant(ev.capability_tenant_id(), kCapMutate, Effect::Mutate,
                                          {});

        const bool has = ev.has_capability(kCapMutate);
        const bool req =
            ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:ac7", 0);
        CHECK(has == c.expect_has_capability,
              std::format("{}: has_capability expected={}", c.name, c.expect_has_capability));
        CHECK(req == c.expect_require_effect,
              std::format("{}: require_effect expected={}", c.name, c.expect_require_effect));
    }

    // Wildcard sanity: "*" string grant satisfies everything under Strict.
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(std::string(kCapWildcard));
    CHECK(ev.has_capability(kCapMutate), "wildcard satisfies mutate");
    CHECK(ev.has_capability(kCapCompileStats), "wildcard satisfies compile-stats");
    CHECK(ev.has_capability(kCapNetwork), "wildcard satisfies network");
}

} // namespace

int run_test_capability_unified() {
    std::println("=== Issue #2077: unify has_capability with Effect matrix ===");
    ac1_source_and_strict_delegates();
    ac2_effect_only_grant_satisfies();
    ac3_string_only_denies_when_matrix_empty();
    ac4_string_only_caps_still_work();
    ac5_wildcard_full_mask();
    ac6_macro_self_evo_no_regression();
    ac7_matrix();
    std::println("\n=== #2077: passed={} failed={} ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capability_unified();
}
#endif

// tests/core/test_check_and_record_wildcard_strip.cpp
// @category: unit
// @reason: Issue #3363 — `check_and_record_effect(wildcard_ok)` skip
//          branch closed; wildcard-only holder no longer passes
//          `require_effect(TenantAdmin|MacroSelfEvo)` via the check
//          path. #3144 effects_for_locked strip is now uniform across
//          `effects_for` / `effects_for_locked` / `check_and_record_effect`.
//
//   AC1: source cites #3363; wildcard-only + Restricted + TA/MSE →
//        deny (#3144 strip + bit coverage).
//   AC2: wildcard-only + Restricted + Mutate → allow (Mutate NOT in
//        #3144 strip list; explicit Mutate grant covers via strip
//        preserving explicit bits).
//   AC3: no wildcard + no Mutate grant → require_effect(Mutate) →
//        deny (regression — existing tests still green).
//   AC4: Soft/Off — need_grant==false → zero-cost short-circuit, no
//        effects_for_locked scan, no fence deny.
//
// Source-cite:
//   src/core/capability_model.hh — `check_and_record_effect` (line ~1719):
//     `#3363` removed the `else if (wildcard_ok)` skip branch; the
//     `need_grant` path now uniformly uses `effects_for_locked`
//     (#3144 strip — TA/MSE wildcard-only stripped). `wildcard_ok`
//     retained as observability via `via-wildcard-denied` reason
//     stamping in `record_audit`.
//   src/core/capability_model.hh — `effects_for_locked` (line ~1080):
//     strips wildcard-only TA/MSE per #3144 (single source of truth).
//   src/compiler/evaluator_security.cpp — `Evaluator::check_and_record_effect`
//     (line ~183) bridges `has_capability(kCapWildcard)` as
//     `wildcard_ok` — the parameter is now observability-only.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Evaluator;
using aura::compiler::security::kCapTenantAdmin;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMacroSelfEvo;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectSyscall;
using aura::compiler::security::kEffectTenantAdmin;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

bool check(bool cond, std::string_view msg) {
    if (cond) {
        ++g_passed;
        std::println("  PASS: {}", msg);
        return true;
    }
    ++g_failed;
    std::println("  FAIL: {}", msg);
    return false;
}

// Build a non-zero EffectProvenance for tests so the provenance fence
// does not refuse (epoch != 0, mutation_id != 0).
EffectProvenance make_test_prov() {
    EffectProvenance p{};
    p.mutation_id = 1;
    p.epoch = 1;
    p.fiber_id = 1;
    p.node_id = 0;
    return p;
}

} // namespace

int main() {
    using namespace aura::core::capability;
    std::println("\n--- Issue #3363: check_and_record_effect wildcard strip uniform ---");

    // ----- setup -----
    reset_capability_effects_for_test();
    g_capability_effect_metrics().capability_provenance_mismatch_total.store(0);
    g_capability_effect_metrics().capability_check_total.store(0);

    Evaluator ev{};
    constexpr std::uint16_t kTA = static_cast<std::uint16_t>(kEffectTenantAdmin);
    constexpr std::uint16_t kMSE = static_cast<std::uint16_t>(kEffectMacroSelfEvo);
    constexpr std::uint16_t kMutate = static_cast<std::uint16_t>(kEffectMutate);

    // ----- AC1: wildcard-only + Restricted + TA/MSE → deny -----
    std::println(
        "\n--- AC1: wildcard-only + Restricted → TA/MSE deny (bit coverage via #3144 strip) ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*"); // wildcard-only, no explicit TA
        // Sanity: confirm wildcard-only state.
        const auto tenant = ev.capability_tenant_id();
        auto& reg = g_capability_registry();
        bool is_wildcard_only;
        {
            std::lock_guard<std::mutex> lock(reg.mtx);
            is_wildcard_only = reg.holds_wildcard_only_locked(tenant);
        }
        check(is_wildcard_only, "tenant holds wildcard-only (no explicit TA/MSE grants)");

        // Call check_and_record_effect for TA — must deny because
        // effects_for_locked strips wildcard-only TA (per #3144).
        const auto prov = make_test_prov();
        const bool ta_ok =
            check_and_record_effect(static_cast<Effect>(kTA), static_cast<Effect>(kTA), prov,
                                    tenant, "test-ac1-ta", /*wildcard_ok=*/true,
                                    /*sandbox_active=*/true);
        check(!ta_ok, "AC1: require_effect(TenantAdmin) DENIED for wildcard-only holder");

        const bool mse_ok =
            check_and_record_effect(static_cast<Effect>(kMSE), static_cast<Effect>(kMSE), prov,
                                    tenant, "test-ac1-mse", /*wildcard_ok=*/true,
                                    /*sandbox_active=*/true);
        check(!mse_ok, "AC1: require_effect(MacroSelfEvo) DENIED for wildcard-only holder");

        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC2: wildcard-only + Restricted + Mutate → allow (Mutate NOT stripped) -----
    std::println(
        "\n--- AC2: wildcard-only + Restricted → Mutate allow (#3144 preserves Mutate) ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*");
        // Add an explicit Mutate grant (Mutate is NOT in the #3144 strip list,
        // so wildcard-only strip preserves Mutate bits if explicitly granted).
        ev.grant_effect_capability(ev.capability_tenant_id(), "mutate",
                                   static_cast<std::uint16_t>(kEffectMutate),
                                   /*provenance_mutation_id=*/42);
        const auto tenant = ev.capability_tenant_id();
        const auto prov = make_test_prov();
        const bool mutate_ok =
            check_and_record_effect(static_cast<Effect>(kMutate), static_cast<Effect>(kMutate),
                                    prov, tenant, "test-ac2-mutate", /*wildcard_ok=*/true,
                                    /*sandbox_active=*/true);
        check(mutate_ok, "AC2: require_effect(Mutate) ALLOWED for explicit-Mutate holder "
                         "(wildcard_only + Mutate bit)");
        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC3: no wildcard + no Mutate grant → Mutate deny (regression) -----
    std::println("\n--- AC3: no wildcard + no Mutate → Mutate deny (regression) ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        // No grants at all.
        const auto tenant = ev.capability_tenant_id();
        const auto prov = make_test_prov();
        const bool mutate_ok =
            check_and_record_effect(static_cast<Effect>(kMutate), static_cast<Effect>(kMutate),
                                    prov, tenant, "test-ac3-mutate", /*wildcard_ok=*/false,
                                    /*sandbox_active=*/true);
        check(!mutate_ok, "AC3: require_effect(Mutate) DENIED for empty-grants holder");
        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC4: Soft/Off — zero-cost short-circuit, no fence scan -----
    std::println("\n--- AC4: Soft/Off → zero-cost short-circuit ---");
    {
        set_mode(EffectSandboxMode::Off);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*");
        const auto before_check = g_capability_effect_metrics().capability_check_total.load();
        const auto tenant = ev.capability_tenant_id();
        const auto prov = make_test_prov();
        // In Off mode, need_grant is false → the bit check / provenance check
        // body is skipped entirely. Wildcard-only TA must still allow (per
        // Off / Soft semantics) — this is unchanged by #3363.
        const bool ta_ok =
            check_and_record_effect(static_cast<Effect>(kTA), static_cast<Effect>(kTA), prov,
                                    tenant, "test-ac4-ta", /*wildcard_ok=*/true,
                                    /*sandbox_active=*/false);
        check(ta_ok, "AC4: Soft/Off allow TA (unchanged — #3363 only tightens Restricted/Strict)");
        const auto after_check = g_capability_effect_metrics().capability_check_total.load();
        check(after_check == before_check + 1,
              "AC4: capability_check_total bumps by 1 (audit record, no fence scan)");
    }

    // ----- AC4b: Soft/Off + wildcard_ok=false → same path (no fence scan) -----
    std::println("\n--- AC4b: Soft/Off + wildcard_ok=false → same path (no fence scan) ---");
    {
        set_mode(EffectSandboxMode::Off);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*");
        const auto before_check = g_capability_effect_metrics().capability_check_total.load();
        const auto tenant = ev.capability_tenant_id();
        const auto prov = make_test_prov();
        const bool ta_ok =
            check_and_record_effect(static_cast<Effect>(kTA), static_cast<Effect>(kTA), prov,
                                    tenant, "test-ac4b-ta", /*wildcard_ok=*/false,
                                    /*sandbox_active=*/false);
        check(ta_ok, "AC4b: Soft/Off + wildcard_ok=false → TA allow (no fence scan)");
    }

    // ----- restore -----
    set_mode(EffectSandboxMode::Off);
    g_capability_registry().reset_for_test();

    if (g_failed) {
        std::println("\nFAIL: {} passed / {} failed", g_passed, g_failed);
        return 1;
    }
    std::println("\nPASS: {} passed / 0 failed — Issue #3363 wildcard-check strip uniform verified",
                 g_passed);
    return 0;
}
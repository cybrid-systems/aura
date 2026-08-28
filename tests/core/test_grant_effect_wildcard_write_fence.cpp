// tests/core/test_grant_effect_wildcard_write_fence.cpp
// @category: unit
// @reason: Issue #3362 — wildcard-only privilege-escalation residual.
//
//   AC1: source cites #3362; same-tenant `grant_effect_capability` under
//        production closes the admin fence for high-risk bits when caller
//        is wildcard-only (`effects_for_locked` has no TenantAdmin after
//        #3144 strip).
//   AC2: same-tenant `grant_effect_capability` rejects wildcard-only +
//        high-risk; `capability_wildcard_write_fence_deny_total` bumps +
//        SE emit + no live TA/MSE bits in by_tenant.
//   AC3: explicit TenantAdmin still allows same-tenant high-risk grants
//        (regression coverage — #2968 AC2 behavior preserved).
//   AC4: `grant_macro_self_evo` with wildcard-only caller → deny +
//        `capability_macro_self_evo_grant_deny_total` bumps + no MSE bit.
//   AC5: Soft/Off — same-tenant self-grant of high-risk bits proceeds with
//        zero added cost (no fence scan, no SE, no deny counter bump).
//   AC6: mid refuse — production (Restricted/Strict) mid=0 grant still
//        refuses per #3090 (independent of the #3362 fence).
//   AC7: `security:grant-effect!` / `security:grant-capability!` /
//        `security:grant-cross-tenant!` language surface rejects
//        wildcard-only caller — but we exercise the underlying path here
//        (the prim wiring is covered by the call-site fence changes
//        in evaluator_primitives_security.cpp).

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Evaluator;
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
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::SecurityEvent;
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

// Helper: build a non-zero mid + epoch + fiber EffectProvenance for tests.
EffectProvenance make_test_prov(std::uint64_t mid) {
    EffectProvenance p{};
    p.mutation_id = mid != 0 ? mid : 1;
    p.epoch = mid != 0 ? mid : 1;
    p.fiber_id = 1;
    p.node_id = 0;
    return p;
}

} // namespace

int main() {
    using namespace aura::core::capability;
    std::println("\n--- Issue #3362: wildcard-write fence for grant_effect_capability ---");

    // ----- setup: reset metrics + ensure Off sandbox for clean state -----
    reset_capability_effects_for_test();
    g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.store(0);
    g_capability_effect_metrics().capability_macro_self_evo_grant_deny_total.store(0);
    g_capability_effect_metrics().capability_grant_mid_refused_total.store(0);

    Evaluator ev{};
    const auto tenant = ev.capability_tenant_id();
    constexpr std::uint16_t kTA = static_cast<std::uint16_t>(kEffectTenantAdmin);
    constexpr std::uint16_t kMSE = static_cast<std::uint16_t>(kEffectMacroSelfEvo);
    constexpr std::uint16_t kMutate = static_cast<std::uint16_t>(kEffectMutate);

    // ----- AC1+AC2: wildcard-only + Restricted + high-risk grant → deny -----
    std::println(
        "\n--- AC1+AC2: wildcard-only cannot synthesize TA via grant_effect_capability ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        // Reset and grant `*` (wildcard-only) to the tenant — no explicit TA.
        g_capability_registry().reset_for_test();
        ev.grant_capability("*");
        // Verify wildcard-only state.
        auto& reg = g_capability_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        check(reg.holds_wildcard_only_locked(tenant),
              "tenant holds wildcard-only (no explicit TA grant)");
        std::uint16_t before_bits = reg.effects_for_locked(tenant);
        check((before_bits & kTA) == 0,
              "effects_for_locked has NO TA after wildcard-only (per #3144 strip)");
        const auto fence_before =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();

        // Call grant_effect_capability with high-risk bits (TA + MSE + Mutate).
        ev.grant_effect_capability(tenant, "tenant-admin",
                                   static_cast<std::uint16_t>(kTA | kMSE | kMutate),
                                   /*provenance_mutation_id=*/42);

        const auto fence_after =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();
        check(fence_after == fence_before + 1,
              "wildcard-write-fence deny counter incremented by 1");

        // Verify NO TA / MSE bits landed in by_tenant (registry write must
        // NOT happen on deny — AC2 string-fence-failure-doesn't-leave-Effect).
        std::uint16_t after_bits = reg.effects_for_locked(tenant);
        check((after_bits & kTA) == 0, "AC2: no TA bit written to by_tenant after fence-deny");
        check((after_bits & kMSE) == 0, "AC2: no MSE bit written to by_tenant after fence-deny");
        check((after_bits & kMutate) == 0,
              "AC2: no Mutate bit written to by_tenant after fence-deny");

        // Verify a SE was emitted with the stable deny reason.
        const auto& ring = g_security_event_ring();
        const auto cur = ring.seq.load(std::memory_order_acquire);
        bool found_reason = false;
        for (auto s = cur; s > 0 && s + 16 > cur && !found_reason; --s) {
            const auto& slot = ring.events[s % ring.events.size()];
            if (slot.kind != 0 /*EffectDeny*/)
                continue;
            const std::string_view r(slot.reason, strnlen(slot.reason, sizeof(slot.reason)));
            if (r == "grant-effect-needs-explicit-tenant-admin") {
                found_reason = true;
                break;
            }
        }
        check(found_reason,
              "SE emit carries stable deny reason 'grant-effect-needs-explicit-tenant-admin'");

        // Reset sandbox for next AC.
        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC3: explicit TA still allows same-tenant high-risk grant -----
    std::println("\n--- AC3: explicit TA still writes high-risk bits ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        ev.grant_capability("tenant-admin"); // explicit TA via string path
        auto& reg = g_capability_registry();
        std::uint16_t before_bits;
        {
            std::lock_guard<std::mutex> lock(reg.mtx);
            before_bits = reg.effects_for_locked(tenant);
        }
        check((before_bits & kTA) != 0,
              "explicit tenant-admin grant yields TA bit in effects_for_locked");
        const auto fence_before =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();

        ev.grant_effect_capability(tenant, "tenant-admin", static_cast<std::uint16_t>(kTA | kMSE),
                                   /*provenance_mutation_id=*/42);

        const auto fence_after =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();
        check(fence_after == fence_before,
              "AC3: explicit-TA holder does NOT bump fence-deny counter");
        std::uint16_t after_bits;
        {
            std::lock_guard<std::mutex> lock(reg.mtx);
            after_bits = reg.effects_for_locked(tenant);
        }
        check((after_bits & kTA) != 0, "AC3: TA bit present after explicit-TA holder grants");
        check((after_bits & kMSE) != 0,
              "AC3: MSE bit present after explicit-TA holder grants TA|MSE");
        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC4: grant_macro_self_evo wildcard-only → deny -----
    std::println("\n--- AC4: grant_macro_self_evo wildcard-only → deny ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*");
        const auto deny_before =
            g_capability_effect_metrics().capability_macro_self_evo_grant_deny_total.load();
        // grant_macro_self_evo(tenant, policy, prov, caller_principal=tenant)
        // — caller_principal=tenant so admin check resolves against the
        // wildcard-only holder (not the default_tenant=0 fallback).
        g_capability_registry().grant_macro_self_evo(tenant, {}, make_test_prov(42), tenant);
        const auto deny_after =
            g_capability_effect_metrics().capability_macro_self_evo_grant_deny_total.load();
        check(deny_after == deny_before + 1,
              "AC4: macro-self-evo deny counter incremented by 1 for wildcard-only caller");
        auto& reg = g_capability_registry();
        std::uint16_t bits;
        {
            std::lock_guard<std::mutex> lock(reg.mtx);
            bits = reg.effects_for_locked(tenant);
        }
        check((bits & kMSE) == 0,
              "AC4: no MSE bit added to tenant after wildcard-only macro-self-evo deny");
        set_mode(EffectSandboxMode::Off);
    }

    // ----- AC5: Soft/Off — no extra cost, same-tenant self-grant unchanged -----
    std::println("\n--- AC5: Soft/Off — no fence cost, same-tenant self-grant proceeds ---");
    {
        set_mode(EffectSandboxMode::Off);
        g_capability_registry().reset_for_test();
        ev.grant_capability("*"); // wildcard-only
        const auto fence_before =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();
        // In Off mode, the same-tenant path short-circuits before any fence
        // scan, so the fence counter must NOT bump and the high-risk bits
        // must land.
        ev.grant_effect_capability(tenant, "tenant-admin",
                                   static_cast<std::uint16_t>(kTA | kMSE | kMutate),
                                   /*provenance_mutation_id=*/42);
        const auto fence_after =
            g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load();
        check(fence_after == fence_before, "AC5: Off mode does NOT bump fence-deny counter");
        auto& reg = g_capability_registry();
        std::uint16_t bits;
        {
            std::lock_guard<std::mutex> lock(reg.mtx);
            bits = reg.effects_for_locked(tenant);
        }
        check((bits & kTA) != 0, "AC5: TA bit lands under Off mode (no fence cost)");
        check((bits & kMSE) != 0, "AC5: MSE bit lands under Off mode (no fence cost)");
    }

    // ----- AC6: mid=0 in Restricted → #3090 refuse (independent of #3362) -----
    std::println("\n--- AC6: mid=0 production grant refused per #3090 ---");
    {
        set_mode(EffectSandboxMode::Restricted);
        g_capability_registry().reset_for_test();
        ev.grant_capability("tenant-admin"); // explicit TA so #3362 fence passes
        const auto mid_refuse_before =
            g_capability_effect_metrics().capability_grant_mid_refused_total.load();
        // mid=0 → production refuse path (#3090) fires BEFORE the registry
        // write. The mid_refused counter must bump; no bits land.
        ev.grant_effect_capability(tenant, "tenant-admin", kTA, /*provenance_mutation_id=*/0);
        const auto mid_refuse_after =
            g_capability_effect_metrics().capability_grant_mid_refused_total.load();
        check(mid_refuse_after == mid_refuse_before + 1,
              "AC6: mid=0 production grant bumps capability_grant_mid_refused_total");
        set_mode(EffectSandboxMode::Off);
    }

    // Final restore.
    set_mode(EffectSandboxMode::Off);
    g_capability_registry().reset_for_test();

    if (g_failed) {
        std::println("\nFAIL: {} passed / {} failed", g_passed, g_failed);
        return 1;
    }
    std::println("\nPASS: {} passed / 0 failed — Issue #3362 wildcard-write fence verified",
                 g_passed);
    return 0;
}
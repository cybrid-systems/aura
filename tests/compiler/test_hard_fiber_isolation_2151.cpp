// @category: unit
// @reason: Issue #2151 — Optional hard-deny on grant_fiber_id mismatch
// (TenantScope isolation). Soft default preserves #2055 same-tenant
// multi-fiber share; hard policy for multi-tenant Strict / env override.
//
//   AC1: hard_fiber_isolation=false → fiber mismatch allow + metric only
//   AC2: hard_fiber_isolation=true → fiber B cannot use grant from fiber A;
//        deny + SecurityEvent reason fiber-grant-mismatch
//   AC3: TenantScope restore does not leave stale principal (RAII + deny)
//   AC4: Wildcard still honors epoch fence; fiber hard-deny after provenance_ok
//   AC5: query:capability-effect-stats exposes hard-fiber-isolation + counts

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <cstdlib>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::effect_fiber_id_or;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::kHardFiberIsolationIssue;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::set_effect_fiber_id_override;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_security_event_ring_for_test();
    reset_audit_wal_for_test();
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Find most recent SecurityEvent with matching reason substring.
bool find_reason(std::string_view needle) {
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    const auto n = total < kSecurityEventRingSize ? total : kSecurityEventRingSize;
    for (std::size_t i = 0; i < n; ++i) {
        const auto idx = (total - 1 - i) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        if (std::string_view(e.reason).find(needle) != std::string_view::npos)
            return true;
    }
    return false;
}

} // namespace

int run_test_hard_fiber_isolation_2151() {
    std::println("=== Issue #2151: optional hard-deny grant_fiber_id mismatch ===");
    CHECK(kHardFiberIsolationIssue == 2151, "issue stamp");

    // ── AC1: soft default — mismatch allows + metric only ──
    {
        std::println("\n--- AC1: soft default (hard_fiber_isolation=false) ---");
        reset_all();
        bump_mutation_epoch(3);
        CHECK(!g_capability_registry().hard_fiber_isolation(), "default hard_fiber off");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict
        ev.set_capability_tenant_id(42);

        // Grant on fiber A (simulated via override).
        set_effect_fiber_id_override(100);
        CHECK(effect_fiber_id_or(0) == 100, "override stamps fiber 100");
        ev.grant_effect_capability(42, "mutate-2151", kEffectMutate, /*prov=*/0);

        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(42, "mutate-2151", g), "grant found");
        CHECK(g.grant_fiber_id == 100, "grant stamped fiber A=100");

        // Effect-check on fiber B — soft path must allow.
        set_effect_fiber_id_override(200);
        const auto mismatch0 = g_capability_effect_metrics().capability_fiber_mismatch_total.load();
        const auto hard0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        const bool ok = ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac1-soft",
                                                   /*target=*/0, /*tenant=*/42, /*mid=*/0);
        CHECK(ok, "AC1: soft mismatch allows");
        CHECK(g_capability_effect_metrics().capability_fiber_mismatch_total.load() > mismatch0,
              "AC1: fiber-mismatch metric advanced");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() == hard0,
              "AC1: hard-deny counter untouched");
        set_effect_fiber_id_override(0);
    }

    // ── AC2: hard isolation — fiber B denied + SecurityEvent ──
    {
        std::println("\n--- AC2: hard_fiber_isolation=true deny + SecurityEvent ---");
        reset_all();
        bump_mutation_epoch(5);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        ev.set_capability_tenant_id(77);
        g_capability_registry().set_hard_fiber_isolation(true);
        CHECK(g_capability_registry().hard_fiber_isolation(), "policy on");

        set_effect_fiber_id_override(11);
        ev.grant_effect_capability(77, "mutate-hard", kEffectMutate, 0);
        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(77, "mutate-hard", g), "grant found");
        CHECK(g.grant_fiber_id == 11, "grant fiber A=11");

        set_effect_fiber_id_override(22);
        const auto hard0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        const auto denied0 = g_capability_effect_metrics().capability_effect_denied_total.load();
        reset_security_event_ring_for_test();

        const bool ok =
            ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac2-hard", 0, 77, 0);
        CHECK(!ok, "AC2: hard mismatch denies");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > hard0,
              "AC2: fiber-hard-deny metric advanced");
        CHECK(g_capability_effect_metrics().capability_effect_denied_total.load() > denied0,
              "AC2: effect denied total advanced");
        CHECK(find_reason("fiber-grant-mismatch"),
              "AC2: SecurityEvent reason fiber-grant-mismatch");

        // Same fiber still allowed.
        set_effect_fiber_id_override(11);
        const bool ok_same =
            ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac2-same", 0, 77, 0);
        CHECK(ok_same, "AC2: same-fiber grant still allows");
        set_effect_fiber_id_override(0);
    }

    // ── AC3: TenantScope restore + hard deny cooperate ──
    {
        std::println("\n--- AC3: TenantScope restore + hard deny ---");
        reset_all();
        bump_mutation_epoch(2);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        g_capability_registry().set_hard_fiber_isolation(true);
        ev.set_capability_tenant_id(1); // outer principal

        {
            Evaluator::TenantScope scope(ev, /*tenant=*/99, "agent-99", /*allow_cross=*/false);
            CHECK(ev.capability_tenant_id() == 99, "scope entered tenant 99");
            set_effect_fiber_id_override(501);
            ev.grant_effect_capability(99, "mutate-scope", kEffectMutate, 0);

            set_effect_fiber_id_override(502);
            const bool denied =
                !ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac3-fiber", 0, 99, 0);
            CHECK(denied, "AC3: hard deny under TenantScope");
        }
        // After scope exit, prior principal restored.
        CHECK(ev.capability_tenant_id() == 1, "AC3: TenantScope restores prior principal");
        set_effect_fiber_id_override(0);
    }

    // ── AC4: wildcard honors epoch fence; fiber hard-deny after provenance ──
    {
        std::println("\n--- AC4: wildcard epoch fence + fiber hard-deny ---");
        reset_all();
        bump_mutation_epoch(10);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        ev.set_capability_tenant_id(55);
        g_capability_registry().set_hard_fiber_isolation(true);

        // Grant mutate on fiber A, then set min_valid past grant epoch.
        set_effect_fiber_id_override(31);
        ev.grant_effect_capability(55, "mutate-fence", kEffectMutate, 0);
        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(55, "mutate-fence", g), "grant");
        const auto ge = g.grant_epoch;
        CHECK(ge != 0, "non-zero grant epoch");

        // Wildcard string grant (legacy "*") so wildcard_ok path is taken.
        ev.grant_capability("*");

        // Fence: min_valid > grant epoch → provenance_ok false even with wildcard.
        g_capability_registry().set_grant_min_valid_epoch(ge + 100);
        set_effect_fiber_id_override(31); // same fiber — fence still wins
        const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
        const bool fence_deny =
            !ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac4-fence", 0, 55, 0);
        CHECK(fence_deny, "AC4: epoch fence denies under wildcard");
        CHECK(g_capability_effect_metrics().capability_epoch_fence_hit_total.load() > fence0,
              "AC4: epoch-fence metric advanced");

        // Clear fence; fiber mismatch under hard isolation still denies.
        g_capability_registry().set_grant_min_valid_epoch(0);
        // Re-grant so grant is fresh after fence tests (same fiber then mismatch).
        set_effect_fiber_id_override(31);
        ev.grant_effect_capability(55, "mutate-fence2", kEffectMutate, 0);
        set_effect_fiber_id_override(99);
        const auto hard0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        const bool fiber_deny =
            !ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac4-fiber", 0, 55, 0);
        CHECK(fiber_deny, "AC4: fiber hard-deny after provenance_ok under wildcard");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > hard0,
              "AC4: hard-deny metric on wildcard path");
        set_effect_fiber_id_override(0);
    }

    // ── AC5: query surface + production defaults wire ──
    {
        std::println("\n--- AC5: query surface + production defaults ---");
        reset_all();

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);

        // Soft: flag 0, counts 0.
        CHECK(href(cs, "schema-2151") == kHardFiberIsolationIssue, "schema-2151");
        CHECK(href(cs, "issue-2151") == 2151, "issue-2151");
        CHECK(href(cs, "hard-fiber-isolation") == 0, "flag off by default");
        CHECK(href(cs, "hard-fiber-isolation-wired") == 1, "wired marker");
        CHECK(href(cs, "fiber-hard-deny") == 0, "hard-deny zero initially");

        // Explicit env on.
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_HARD_FIBER_ISOLATION", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(), "env=1 enables hard fiber");

        // Explicit env off wins over multi-tenant.
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        set_env("AURA_MULTI_TENANT", "1");
        set_env("AURA_HARD_FIBER_ISOLATION", "0");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "env=0 overrides multi-tenant Strict");

        // Multi-tenant Strict without env → hard on.
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(),
              "multi-tenant Strict enables hard fiber by default");

        // AURA_SANDBOX=off → soft.
        reset_all();
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_MULTI_TENANT", "1");
        set_env("AURA_HARD_FIBER_ISOLATION", "1");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "sandbox=off forces soft (tests safe)");

        // After a hard deny, query counts advance.
        reset_all();
        CompilerService cs2;
        auto& ev2 = cs2.evaluator();
        ev2.set_effect_sandbox_mode(2);
        ev2.set_capability_tenant_id(8);
        g_capability_registry().set_hard_fiber_isolation(true);
        set_effect_fiber_id_override(1);
        ev2.grant_effect_capability(8, "m", kEffectMutate, 0);
        set_effect_fiber_id_override(2);
        (void)ev2.check_and_record_effect(kEffectMutate, kEffectMutate, "q", 0, 8, 0);
        CHECK(href(cs2, "hard-fiber-isolation") == 1, "query flag on");
        CHECK(href(cs2, "fiber-hard-deny") >= 1, "query fiber-hard-deny count");
        CHECK(href(cs2, "fiber-mismatch") >= 1, "query fiber-mismatch count");
        set_effect_fiber_id_override(0);
    }

    std::println("\n=== #2151 hard fiber isolation: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hard_fiber_isolation_2151();
}
#endif

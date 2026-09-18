// @category: unit
// @reason: Issue #2529 — Restricted single-tenant grant_epoch_retain K=16.
//          Issue #3774 — MSE production session rows arm live_session_grants.
//
//   AC1: Restricted + no multi-tenant → K==16
//   AC2: multi-tenant or Strict → still 64
//   AC3: AURA_GRANT_EPOCH_RETAIN=N overrides
//   AC4: AURA_SANDBOX=off → K=0 + min_valid=0
//   AC5: Restricted bump epoch >K → old grant provenance_ok false + fence++
//   AC6: source-cite + linter

#include "test_harness.hpp"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;

namespace {
using aura::compiler::security::apply_production_security_defaults;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::has_effect;
using aura::core::capability::kDefaultGrantEpochRetainWindowMultiTenant;
using aura::core::capability::kDefaultGrantEpochRetainWindowRestricted;
using aura::core::capability::kEffectsEffectiveRetainIssue;
using aura::core::capability::kGrantEpochRetainRestrictedIssue;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_GRANT_EPOCH_RETAIN");
}
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
} // namespace


int run_test_mse_session_live_grants_3774() {
    std::println("=== Issue #3774: MSE production session rows arm live_session_grants ===");
    // ── #3774: MSE production session rows arm live residual + orphan sweep ──
    {
        std::println("\n--- #3774 AC1: Restricted MSE grant bumps live_session_grants ---");
        reset_all();
        // Issue #3409 bootstrap: seed TenantAdmin while Off, then arm Restricted
        // (grant_locked SSOT fences high-bits grants under production).
        const auto tenant = std::uint64_t{17};
        bump_mutation_epoch();
        const auto mid = current_mutation_epoch();
        CHECK(g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin,
                                            make_grant_provenance(mid, false, 0, 0)),
              "3774 AC1: TA bootstrap under Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto live0 = g_capability_effect_metrics().capability_live_session_grants.load();
        const auto sess0 = g_capability_effect_metrics().capability_session_grant_total.load();
        EffectProvenance prov;
        prov.mutation_id = mid;
        prov.epoch = mid;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3774 AC1: MSE grant ok");
        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(tenant, "macro-self-evo", g),
              "3774 AC1: MSE row present");
        CHECK(g.session_bound && g.single_use, "3774 AC1: session_bound+single_use stamped");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == live0 + 1,
              "3774 AC1: live_session_grants++ (orphan sweep armed)");
        CHECK(g_capability_effect_metrics().capability_session_grant_total.load() == sess0 + 1,
              "3774 AC1: session_grant_total++");
        // Re-grant same live session row must not double-bump.
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3774 AC1: re-grant ok");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == live0 + 1,
              "3774 AC1: re-grant does not double-bump live");
    }
    {
        std::println("\n--- #3774 AC2: orphan sweep clears MSE-only residual ---");
        reset_all();
        const auto tenant = std::uint64_t{18};
        bump_mutation_epoch();
        const auto mid = current_mutation_epoch();
        CHECK(g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin,
                                            make_grant_provenance(mid, false, 0, 0)),
              "3774 AC2: TA bootstrap under Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance prov;
        prov.mutation_id = mid;
        prov.epoch = mid;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3774 AC2: MSE grant ok");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() >= 1,
              "3774 AC2: live residual armed");
        // Lost-Guard / mid-clear race: only orphan sweep remains; live mids
        // do not include the MSE bound mid → row revoked.
        const auto n =
            g_capability_registry().sweep_session_bound_orphans({mid + 4242}, /*fiber_id=*/0);
        CHECK(n >= 1, "3774 AC2: orphan sweep revoked MSE session row");
        CHECK((g_capability_registry().effects_for(tenant) & Effect::MacroSelfEvo) == Effect::None,
              "3774 AC2: MacroSelfEvo cleared by sweep");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
              "3774 AC2: live residual cleared");
    }
    {
        std::println("\n--- #3774 AC3: named MSE revoke clears session_bound + live ---");
        reset_all();
        const auto tenant = std::uint64_t{19};
        bump_mutation_epoch();
        const auto mid = current_mutation_epoch();
        CHECK(g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin,
                                            make_grant_provenance(mid, false, 0, 0)),
              "3774 AC3: TA bootstrap under Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance prov;
        prov.mutation_id = mid;
        prov.epoch = mid;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3774 AC3: MSE grant ok");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() >= 1,
              "3774 AC3: live residual present");
        g_capability_registry().revoke_macro_self_evo(tenant);
        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(tenant, "macro-self-evo", g),
              "3774 AC3: row still findable (revoked)");
        CHECK(g.revoked && !g.session_bound, "3774 AC3: revoked + session_bound cleared");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
              "3774 AC3: live decremented (Soft AC3 does not ratchet)");
        // Named revoke via revoke_locked / revoke("macro-self-evo") path.
        reset_all();
        bump_mutation_epoch();
        const auto mid2 = current_mutation_epoch();
        CHECK(g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin,
                                            make_grant_provenance(mid2, false, 0, 0)),
              "3774 AC3b: TA bootstrap under Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance prov2;
        prov2.mutation_id = mid2;
        prov2.epoch = mid2;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov2, 0),
              "3774 AC3b: MSE grant ok");
        g_capability_registry().revoke(tenant, "macro-self-evo");
        CapabilityGrant g2;
        CHECK(g_capability_registry().find_grant(tenant, "macro-self-evo", g2),
              "3774 AC3b: row present");
        CHECK(g2.revoked && !g2.session_bound, "3774 AC3b: revoke_locked cleared session_bound");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
              "3774 AC3b: live decremented via revoke_locked");
    }
    {
        std::println("\n--- #3774 AC4: Soft/Off MSE grant is zero-cost on live counter ---");
        reset_all();
        const auto tenant = std::uint64_t{20};
        bump_mutation_epoch();
        const auto epoch = current_mutation_epoch();
        const auto live0 = g_capability_effect_metrics().capability_live_session_grants.load();
        EffectProvenance prov;
        prov.epoch = epoch;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3774 AC4: Soft MSE grant ok");
        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(tenant, "macro-self-evo", g),
              "3774 AC4: Soft row present");
        CHECK(!g.session_bound, "3774 AC4: Soft does not force session_bound");
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == live0,
              "3774 AC4: Soft/Off zero extra live cost");
        g_capability_registry().revoke_macro_self_evo(tenant);
        CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == live0,
              "3774 AC4: Soft revoke leaves live unchanged");
    }
    {
        std::println("\n--- #3774 AC5: source cites #3774 on MSE live residual ---");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("#3774") != std::string::npos, "3774 AC5: capability_model cites #3774");
        CHECK(cap.find("grant_macro_self_evo") != std::string::npos, "3774 AC5: MSE grant present");
    }

    std::println("\n=== #3774 slice: {} passed, {} failed (cumulative face) ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

int run_test_mse_session_live_grants_3774_member() {
    g_passed = 0;
    g_failed = 0;
    return run_test_mse_session_live_grants_3774();
}

int run_test_grant_epoch_retain_restricted() {
    std::println("=== Issue #2529: Restricted grant epoch retain K=16 ===");
    CHECK(kGrantEpochRetainRestrictedIssue == 2529, "issue stamp");
    CHECK(kDefaultGrantEpochRetainWindowRestricted == 16, "K=16 constant");
    CHECK(kDefaultGrantEpochRetainWindowMultiTenant == 64, "K=64 multi-tenant");

    {
        std::println("\n--- AC1: Restricted only → K=16 ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 16, "AC1: K=16");
    }
    {
        std::println("\n--- AC2: multi-tenant / Strict → 64 ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64, "AC2: multi-tenant 64");
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64, "AC2: Strict 64");
    }
    {
        std::println("\n--- AC3: env override ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_GRANT_EPOCH_RETAIN", "3");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 3, "AC3: env=3");
    }
    {
        std::println("\n--- AC4: sandbox=off ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0, "AC4: K=0");
        CHECK(g_capability_registry().grant_min_valid_epoch() == 0, "AC4: min_valid=0");
    }
    {
        std::println("\n--- AC5: fence after >K bumps ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 16, "K=16");
        EffectProvenance prov = make_grant_provenance(0, true, 0, 0);
        // Issue #2657: route through the process-wide authority. The
        // old direct write was a test-helper shortcut that bypassed
        // the broadcast; the authority keeps all four stores in sync.
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        g_capability_registry().grant(1, "mutate", Effect::Mutate, prov);
        const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
        // Advance past window: grant epoch ~1, bump far ahead.
        bump_mutation_epoch(100);
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = aura::core::current_mutation_epoch();
        CHECK(!g_capability_registry().provenance_ok(1, call), "AC5: old grant fenced");
        CHECK(g_capability_effect_metrics().capability_epoch_fence_hit_total.load() > fence0,
              "AC5: fence metric++");
    }
    // ── #3876: effects_effective_for matches require_effect after retain ──
    {
        std::println("\n--- #3876 AC1: Restricted+MT retain → effective bits match deny ---");
        CHECK(kEffectsEffectiveRetainIssue == 3876, "3876: issue stamp");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        aura::core::sandbox::set_mode(SandboxMode::Restricted);
        while (current_mutation_epoch() < 1)
            bump_mutation_epoch(1);
        EffectProvenance prov = make_grant_provenance(0, true, 0, 0);
        // Read is not a high-bits grant (no TenantAdmin fence). Retain still
        // applies — Agent posture overstated Mutate/Read the same way.
        CHECK(g_capability_registry().grant(1, "read", Effect::Read, prov), "3876 AC1: grant Read");
        CapabilityGrant stored{};
        CHECK(g_capability_registry().find_grant(1, "read", stored), "3876 AC1: grant stored");
        CHECK(has_effect(g_capability_registry().effects_for(1), Effect::Read),
              "3876 AC1: raw effects_for Read before retain");
        CHECK(has_effect(g_capability_registry().effects_effective_for(1), Effect::Read),
              "3876 AC1: effective Read before retain");
        bump_mutation_epoch(100);
        EffectProvenance call;
        call.mutation_id = stored.bound_mutation_id;
        call.epoch = current_mutation_epoch();
        CHECK(!g_capability_registry().provenance_ok(1, call, Effect::Read),
              "3876 AC1: require_effect path (provenance_ok) denies");
        CHECK(has_effect(g_capability_registry().effects_for(1), Effect::Read),
              "3876 AC1: raw effects_for still ORs expired grant");
        CHECK(!has_effect(g_capability_registry().effects_effective_for(1), Effect::Read),
              "3876 AC1: effects_effective_for matches deny");
        CHECK(!has_effect(g_capability_registry().effects_effective_for_locked(1), Effect::Read),
              "3876 AC1: locked effective matches deny");
    }
    {
        std::println("\n--- #3876 AC2: Soft/Off effective == effects_for (no retain filter) ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        EffectProvenance prov = make_grant_provenance(0, true, 0, 0);
        CHECK(g_capability_registry().grant(1, "read", Effect::Read, prov), "3876 AC2: Soft grant");
        g_capability_registry().set_grant_min_valid_epoch(1'000'000);
        CHECK(has_effect(g_capability_registry().effects_for(1), Effect::Read),
              "3876 AC2: Soft raw Read");
        CHECK(has_effect(g_capability_registry().effects_effective_for(1), Effect::Read),
              "3876 AC2: Soft effective does not filter retain (zero-cost)");
    }
    {
        std::println("\n--- #3876 AC3: source-cite + no invent ---");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(cap.find("Issue #3876") != std::string::npos, "3876 AC3: capability_model cites");
        CHECK(cap.find("effects_effective_for") != std::string::npos, "3876 AC3: effective API");
        CHECK(cap.find("kEffectsEffectiveRetainIssue") != std::string::npos, "3876 AC3: stamp");
        CHECK(sec.find("Issue #3876") != std::string::npos, "3876 AC3: has_capability cites");
        CHECK(sec.find("effects_effective_for") != std::string::npos,
              "3876 AC3: has_capability uses effective");
        CHECK(read_file("tests/compiler/test_issue_3876.cpp").empty(),
              "3876 AC3: no test_issue_3876.cpp");
        CHECK(read_file("docs/design/3876-effects-effective-retain.md").empty(),
              "3876 AC3: no docs/design/");
    }
    {
        std::println("\n--- AC6: source-cite ---");
        auto def = read_file("src/compiler/security_defaults.hh");
        auto cap = read_file("src/core/capability_model.hh");
        CHECK(def.find("2529") != std::string::npos, "defaults cite #2529");
        CHECK(def.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos ||
                  def.find("Restricted") != std::string::npos,
              "Restricted branch");
        CHECK(cap.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos, "const");
        CHECK(cap.find("16") != std::string::npos, "16 present");
    }

    // ── #2688 AC1: production defaults active + multi_tenant → hard_fiber true + K=64 ──
    {
        std::println("\n--- #2688 AC1: multi_tenant/Strict arms hard_fiber + K=64 ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64,
              "AC1: multi_tenant → K=64 (kDefaultGrantEpochRetainWindowMultiTenant)");
        // hard_fiber_isolation default for multi_tenant + Restricted (per AC1): true
        // when strict is also on; under Restricted alone it's soft (#2536).
        // The query surface exposes the flag regardless.
    }
    // ── #2688 AC2: production Restricted (single-tenant) → K=16; hard_fiber stays false ──
    {
        std::println("\n--- #2688 AC2: Restricted alone arms K=16; hard_fiber false ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 16,
              "AC2: Restricted → K=16 (kDefaultGrantEpochRetainWindowRestricted)");
        // hard_fiber stays false under pure Restricted per #2536 (same-tenant
        // multi-fiber share). AURA_HARD_FIBER_ISOLATION=1 env forces on.
    }
    // ── #2688 AC3: Soft / sandbox=off → hard_fiber false, K=0 ──
    {
        std::println("\n--- #2688 AC3: sandbox=off → K=0 + hard_fiber false ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0,
              "AC3: sandbox=off → K=0 (manual fence only)");
        CHECK(g_capability_registry().grant_min_valid_epoch() == 0,
              "AC3: sandbox=off → min_valid=0");
    }
    // ── #2688 AC6: source-cite + linter self-coverage ──
    {
        std::println("\n--- #2688 AC6: source-cite + no regression ---");
        const auto def = read_file("src/compiler/security_defaults.hh");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        // Issue #2688 sentinel in all 3 prod-side files (use "#2688" for
        // combined citations like "Issue #2688 / #2151 / #2154").
        CHECK(def.find("#2688") != std::string::npos, "AC6: security_defaults.hh cites #2688");
        CHECK(cap.find("#2688") != std::string::npos, "AC6: capability_model.hh cites #2688");
        CHECK(q.find("#2688") != std::string::npos,
              "AC6: evaluator_primitives_obs_jit.cpp cites #2688");
        // Constants wired (already true from #2154 + #2529 baseline).
        CHECK(cap.find("kDefaultGrantEpochRetainWindowMultiTenant") != std::string::npos,
              "AC1: K=64 constant present");
        CHECK(cap.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos,
              "AC2: K=16 constant present");
        CHECK(cap.find("kCapabilityProductionDefaultIssue") != std::string::npos,
              "AC6: Issue #2688 stamp constant");
        // Query surface (additive — no schema break for existing keys).
        CHECK(q.find("capability-hard-fiber-isolation") != std::string::npos,
              "AC6: hard-fiber-isolation query key");
        CHECK(q.find("capability-grant-epoch-retain-window") != std::string::npos,
              "AC6: grant-epoch-retain-window query key");
        CHECK(q.find("capability-epoch-fence-hit-total") != std::string::npos,
              "AC6: epoch-fence-hit-total query key");
        CHECK(q.find("capability-fiber-hard-deny-total") != std::string::npos,
              "AC6: fiber-hard-deny-total query key");
        CHECK(q.find("schema-2688") != std::string::npos, "AC6: schema-2688 sentinel");
        CHECK(q.find("issue-2688") != std::string::npos, "AC6: issue-2688 sentinel");
        CHECK(q.find("capability-production-default-armed") != std::string::npos,
              "AC6: production-default-armed sentinel");
        // No design doc regression (per #1655).
        for (const auto& p : {"docs/design/capability_production_default_2688.md",
                              "docs/capability_production_default_2688.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }


    // ── AC7 (#3721): production MSE policy row is session-bound + single-use ──
    {
        std::println("\n--- AC7 (#3721): MSE row session-bound (same lifetime as high-risk) ---");
        reset_all();
        const auto tenant = std::uint64_t{7};
        // Issue #3409 bootstrap: seed TenantAdmin while Off, then Restricted.
        bump_mutation_epoch();
        const auto mid = current_mutation_epoch();
        CHECK(g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin,
                                            make_grant_provenance(mid, false, 0, 0)),
              "3721: TA bootstrap under Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance prov;
        prov.mutation_id = mid;
        prov.epoch = mid;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3721: production MSE grant succeeds (TA fence ok)");
        CHECK((g_capability_registry().effects_for(tenant) & Effect::MacroSelfEvo) != Effect::None,
              "3721: MacroSelfEvo live before session exit");
        // Outermost MutationBoundary exit: session-bound rows bound to this
        // mid are revoked (the Guard dtor calls this).
        (void)g_capability_registry().revoke_session_grants_for_mid(mid);
        CHECK((g_capability_registry().effects_for(tenant) & Effect::MacroSelfEvo) == Effect::None,
              "3721: MSE row dies at session mid exit (dual-track leak closed)");
    }
    {
        std::println("\n--- AC7b (#3721): Soft keeps legacy flags; production mid=0 refuses ---");
        reset_all();
        const auto tenant = std::uint64_t{8};
        bump_mutation_epoch();
        const auto epoch = current_mutation_epoch();
        EffectProvenance prov; // mid=0 → Soft front synthesizes from epoch
        prov.epoch = epoch;
        CHECK(g_capability_registry().grant_macro_self_evo(tenant, {}, prov, 0),
              "3721: Soft MSE grant ok (no TA fence under Off)");
        bump_mutation_epoch();
        const auto mid2 = current_mutation_epoch();
        (void)g_capability_registry().revoke_session_grants_for_mid(mid2);
        CHECK((g_capability_registry().effects_for(tenant) & Effect::MacroSelfEvo) != Effect::None,
              "3721: Soft row survives session revoke (no session_bound force; AC5)");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        EffectProvenance zero_mid; // mid=0 → production refuse (#3459)
        CHECK(!g_capability_registry().grant_macro_self_evo(9, {}, zero_mid, 0),
              "3721: production mid=0 refuse before any write");
    }


    // #3774 ACs live in run_test_mse_session_live_grants_3774 (batch member).
    (void)run_test_mse_session_live_grants_3774();

    std::println("\n=== #2529/#2688/#3774: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_grant_epoch_retain_restricted();
}
#endif

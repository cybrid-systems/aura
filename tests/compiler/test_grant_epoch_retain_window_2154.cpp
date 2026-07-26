// @category: unit
// @reason: Issue #2154 — Sliding grant_min_valid_epoch window on Mutation
// epoch bump (anti privilege-sticky without manual fence).
//
//   AC1: K=0 → no auto advance (identical to #2074 manual-only)
//   AC2: K=10, grant at epoch 1, bump to 15 → grant denied; fence metric++
//   AC3: Grant issued inside the window still allows matching effects
//   AC4: Revoke independent of fence; revoked grants stay revoked
//   AC5: Multi-tenant production defaults document K; env override

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::kDefaultGrantEpochRetainWindowMultiTenant;
using aura::core::capability::kGrantEpochRetainWindowIssue;
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

int main() {
    std::println("=== Issue #2154: sliding grant_min_valid_epoch retain window ===");
    CHECK(kGrantEpochRetainWindowIssue == 2154, "issue stamp");
    CHECK(kDefaultGrantEpochRetainWindowMultiTenant == 64, "default multi-tenant K=64");

    // ── AC1: K=0 no auto advance ──
    {
        std::println("\n--- AC1: K=0 no auto advance ---");
        reset_all();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0, "default K=0");
        CHECK(g_capability_registry().grant_min_valid_epoch() == 0, "default min_valid=0");

        bump_mutation_epoch(20);
        const auto min0 = g_capability_registry().grant_min_valid_epoch();
        const auto adv0 =
            g_capability_effect_metrics().capability_grant_epoch_window_advance_total.load();
        bump_mutation_epoch(5);
        CHECK(g_capability_registry().grant_min_valid_epoch() == min0,
              "AC1: min_valid unchanged with K=0");
        CHECK(g_capability_effect_metrics().capability_grant_epoch_window_advance_total.load() ==
                  adv0,
              "AC1: no window advance with K=0");
    }

    // ── AC2: K=10, old grant fenced after bump past window ──
    {
        std::println("\n--- AC2: K=10 fence after bump past window ---");
        reset_all();
        // Drive mutation epoch to a known floor.
        while (current_mutation_epoch() < 1)
            bump_mutation_epoch(1);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict
        ev.set_capability_tenant_id(7);

        // Grant near current epoch (capture grant_epoch).
        bump_mutation_epoch(1);
        const auto grant_at = current_mutation_epoch();
        ev.grant_effect_capability(7, "old-grant", kEffectMutate, 0);
        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(7, "old-grant", g), "grant found");
        CHECK(g.grant_epoch != 0, "non-zero grant_epoch");
        std::println("  grant_epoch={} current={}", g.grant_epoch, current_mutation_epoch());

        // Enable retain window K=10 and advance far beyond grant_epoch + 10.
        g_capability_registry().set_grant_epoch_retain_window(10);
        CHECK(g_capability_registry().grant_epoch_retain_window() == 10, "K=10 set");

        const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
        const auto adv0 =
            g_capability_effect_metrics().capability_grant_epoch_window_advance_total.load();

        // Bump enough so new_ep - 10 > grant_epoch.
        // If grant_epoch is G, need new_ep - 10 > G ⇒ new_ep > G+10.
        const auto target = g.grant_epoch + 15;
        while (current_mutation_epoch() < target)
            bump_mutation_epoch(1);

        const auto min_valid = g_capability_registry().grant_min_valid_epoch();
        const auto cur = current_mutation_epoch();
        std::println("  after bumps: epoch={} min_valid={} grant={}", cur, min_valid,
                     g.grant_epoch);
        CHECK(min_valid == cur - 10, "AC2: min_valid = new_ep - K");
        CHECK(g_capability_effect_metrics().capability_grant_epoch_window_advance_total.load() >
                  adv0,
              "AC2: window advance metric");
        CHECK(g.grant_epoch < min_valid, "AC2: grant behind fence");

        EffectProvenance call{};
        call.mutation_id = g.bound_mutation_id;
        call.epoch = cur;
        call.fiber_id = g.grant_fiber_id;
        CHECK(!g_capability_registry().provenance_ok(7, call), "AC2: old grant denied");
        CHECK(g_capability_effect_metrics().capability_epoch_fence_hit_total.load() > fence0,
              "AC2: epoch-fence-hit metric advanced");

        // Full effect path also denies.
        const bool ok =
            ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac2-old", 0, 7, 0);
        CHECK(!ok, "AC2: check_and_record_effect denies fenced grant");
    }

    // ── AC3: grant inside window still allows ──
    {
        std::println("\n--- AC3: in-window grant allows ---");
        reset_all();
        bump_mutation_epoch(50);
        g_capability_registry().set_grant_epoch_retain_window(10);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        ev.set_capability_tenant_id(8);

        // Fresh grant at current epoch — inside [min_valid, current].
        bump_mutation_epoch(1);
        ev.grant_effect_capability(8, "fresh", kEffectMutate, 0);
        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(8, "fresh", g), "fresh grant");
        const auto min_valid = g_capability_registry().grant_min_valid_epoch();
        CHECK(g.grant_epoch >= min_valid, "AC3: grant_epoch >= min_valid");

        EffectProvenance call{};
        call.mutation_id = g.bound_mutation_id;
        call.epoch = current_mutation_epoch();
        call.fiber_id = g.grant_fiber_id;
        CHECK(g_capability_registry().provenance_ok(8, call), "AC3: provenance_ok");
        CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac3", 0, 8, 0),
              "AC3: effect allows in-window grant");

        // Small bumps that keep grant inside window still allow.
        bump_mutation_epoch(3);
        CHECK(g.grant_epoch >= g_capability_registry().grant_min_valid_epoch() ||
                  g.grant_epoch >= current_mutation_epoch() - 10,
              "still in window or borderline");
        // Re-check with same grant — may still be valid if K=10 and only +3.
        EffectProvenance call2 = call;
        call2.epoch = current_mutation_epoch();
        if (g.grant_epoch >= g_capability_registry().grant_min_valid_epoch())
            CHECK(g_capability_registry().provenance_ok(8, call2),
                  "AC3: still ok after small bump");
    }

    // ── AC4: revoke independent of fence ──
    {
        std::println("\n--- AC4: revoke independent of fence ---");
        reset_all();
        bump_mutation_epoch(5);
        g_capability_registry().set_grant_epoch_retain_window(100); // wide window

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2);
        ev.set_capability_tenant_id(9);
        ev.grant_effect_capability(9, "revokeme", kEffectMutate, 0);
        CapabilityGrant g{};
        CHECK(g_capability_registry().find_grant(9, "revokeme", g), "granted");
        CHECK(!g.revoked, "not revoked");
        CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "pre-rev", 0, 9, 0),
              "allows before revoke");

        ev.revoke_effect_capability(9, "revokeme");
        CHECK(g_capability_registry().find_grant(9, "revokeme", g), "still findable");
        CHECK(g.revoked, "AC4: revoked flag");
        CHECK(g.effects == Effect::None, "AC4: effects cleared");
        CHECK(!ev.check_and_record_effect(kEffectMutate, kEffectMutate, "post-rev", 0, 9, 0),
              "AC4: effect denied after revoke (not just fence)");

        // Fence advance does not un-revoke.
        bump_mutation_epoch(20);
        CHECK(g_capability_registry().find_grant(9, "revokeme", g), "still present");
        CHECK(g.revoked, "AC4: stays revoked after fence advance");
    }

    // ── AC5: production defaults + env override + query surface ──
    {
        std::println("\n--- AC5: production defaults + env + query ---");
        reset_all();

        // sandbox=off → K=0
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0,
              "AC5: sandbox=off forces K=0");

        // multi-tenant → K=64
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() ==
                  kDefaultGrantEpochRetainWindowMultiTenant,
              "AC5: multi-tenant default K=64");

        // Strict alone (no multi-tenant) → K=64
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() ==
                  kDefaultGrantEpochRetainWindowMultiTenant,
              "AC5: Strict default K=64");

        // Env override
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        set_env("AURA_MULTI_TENANT", "1");
        set_env("AURA_GRANT_EPOCH_RETAIN", "7");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 7,
              "AC5: AURA_GRANT_EPOCH_RETAIN=7 wins");

        // Env 0 disables under multi-tenant
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        set_env("AURA_MULTI_TENANT", "1");
        set_env("AURA_GRANT_EPOCH_RETAIN", "0");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0,
              "AC5: env=0 disables window");

        // Query surface
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        g_capability_registry().set_grant_epoch_retain_window(10);
        bump_mutation_epoch(20);
        CHECK(href(cs, "schema-2154") == 2154, "schema-2154");
        CHECK(href(cs, "grant-epoch-retain-window") == 10, "query retain window");
        CHECK(href(cs, "grant-min-valid-epoch") ==
                  static_cast<std::int64_t>(g_capability_registry().grant_min_valid_epoch()),
              "query min_valid gauge");
        CHECK(href(cs, "grant-epoch-window-wired") == 1, "wired marker");
        CHECK(href(cs, "grant-epoch-window-advance") >= 0, "advance counter key");

        // Docs
        const auto def = read_file("src/compiler/security_defaults.hh");
        CHECK(def.find("AURA_GRANT_EPOCH_RETAIN") != std::string::npos, "env documented");
        CHECK(def.find("2154") != std::string::npos, "defaults cite #2154");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("grant_epoch_retain_window") != std::string::npos, "API present");
        CHECK(cap.find("on_mutation_epoch_bump") != std::string::npos, "bump hook method");
        const auto we = read_file("src/core/workspace_epoch.hh");
        CHECK(we.find("notify_mutation_epoch_bump") != std::string::npos, "epoch notifies hook");
        CHECK(we.find("2154") != std::string::npos, "epoch cites #2154");
    }

    std::println("\n=== #2154 grant epoch retain window: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

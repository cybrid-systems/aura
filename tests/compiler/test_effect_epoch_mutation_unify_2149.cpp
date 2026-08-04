// @category: unit
// @reason: Issue #2149 — Unify effect-check epoch with grant Mutation epoch
// (close Bridge split). Security provenance uses Mutation only.
//
//   AC1: check_and_record_effect stamps EffectProvenance.epoch from
//        current_mutation_epoch() (never Bridge as primary)
//   AC2: Isolated Bridge bump does not flip allow↔deny for a valid grant
//   AC3: grant_min_valid_epoch fence still fires when Mutation advances
//   AC4: #2055 grant_epoch_bound / fiber metrics + schema-2055 unchanged
//   AC5: SecurityEvent / TypedMutation still joinable by mutation_id;
//        schema-2149 additive on query:capability-effect-stats

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"

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
using aura::compiler::Evaluator;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::bump_bridge_epoch;
using aura::core::bump_mutation_epoch;
using aura::core::current_bridge_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectAuditEntry;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::kEffectEpochUnifyIssue;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
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
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

// ── AC1: effect path stamps Mutation epoch ───────────────────
static void ac1_effect_stamps_mutation() {
    std::println("\n--- AC1: check_and_record_effect stamps Mutation epoch ---");
    reset_all();
    // Divergence setup: Mutation and Bridge intentionally differ.
    bump_mutation_epoch(20);
    bump_bridge_epoch(50);
    const auto me = current_mutation_epoch();
    const auto be = current_bridge_epoch();
    CHECK(me != 0 && be != 0, "epochs non-zero");
    CHECK(me != be, "Mutation and Bridge diverged for the test");

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(100);
    ev.grant_effect_capability(100, "mutate-2149", kEffectMutate, /*prov=*/0);

    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(100, "mutate-2149", g), "grant found");
    CHECK(g.grant_epoch == me || g.grant_epoch == 1, "grant_epoch = Mutation");

    // Effect check must stamp Mutation (audit ring / SecurityEvent).
    const bool ok = ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac1-effect", 0, 100,
                                               g.bound_mutation_id);
    CHECK(ok, "effect allowed under grant");

    // Latest capability audit entry carries Mutation epoch.
    // Issue #2425: load via try_load_latest_audit (published slot).
    auto& reg = g_capability_registry();
    const auto seq = reg.load_audit_seq();
    CHECK(seq > 0, "audit written");
    EffectAuditEntry entry{};
    CHECK(reg.try_load_latest_audit(entry), "AC1: latest audit published");
    std::println("  audit.prov.epoch={} mutation={} bridge={}", entry.prov.epoch, me, be);
    CHECK(entry.prov.epoch == me || entry.prov.epoch == 1, "AC1: audit epoch = Mutation");
    CHECK(entry.prov.epoch != be || me == be, "AC1: audit epoch is not Bridge-primary");

    // Source cites #2149
    auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("#2149") != std::string::npos, "evaluator_security cites #2149");
    CHECK(sec.find("current_mutation_epoch()") != std::string::npos,
          "stamps current_mutation_epoch");
}

// ── AC2: Bridge-only bump does not flip allow/deny ───────────
static void ac2_bridge_bump_no_flip() {
    std::println("\n--- AC2: Bridge-only bump keeps valid grant allowed ---");
    reset_all();
    bump_mutation_epoch(5);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(200);
    ev.grant_effect_capability(200, "bridge-iso", kEffectMutate, 0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(200, "bridge-iso", g), "grant");
    const auto mid = g.bound_mutation_id;

    CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "pre-bridge", 0, 200, mid),
          "allow before Bridge bump");

    // Bump ONLY Bridge — Mutation (and grant_epoch) stay put.
    const auto me_before = current_mutation_epoch();
    bump_bridge_epoch(100);
    CHECK(current_mutation_epoch() == me_before, "Mutation unchanged");
    CHECK(current_bridge_epoch() != me_before, "Bridge diverged");

    CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "post-bridge", 0, 200, mid),
          "AC2: still allow after Bridge-only bump");
    EffectProvenance call{};
    call.mutation_id = mid;
    call.epoch = current_mutation_epoch();
    CHECK(g_capability_registry().provenance_ok(200, call), "provenance_ok after Bridge bump");
}

// ── AC3: grant_min_valid_epoch fence still works ─────────────
static void ac3_min_valid_fence() {
    std::println("\n--- AC3: grant_min_valid_epoch fence on Mutation ---");
    reset_all();
    bump_mutation_epoch(10);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict
    ev.set_capability_tenant_id(300);
    ev.grant_effect_capability(300, "fence-2149", kEffectMutate, 0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(300, "fence-2149", g), "granted");
    const auto ge = g.grant_epoch;
    g_capability_registry().set_grant_min_valid_epoch(ge + 1);

    EffectProvenance call{};
    call.mutation_id = g.bound_mutation_id;
    call.epoch = current_mutation_epoch();
    const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
    CHECK(!g_capability_registry().provenance_ok(300, call), "expired grant denied");
    CHECK(g_capability_effect_metrics().capability_epoch_fence_hit_total.load() > fence0,
          "epoch-fence-hits advanced");

    // Full effect path also denies under Strict + expired grant.
    CHECK(!ev.check_and_record_effect(kEffectMutate, kEffectMutate, "fence-deny", 0, 300,
                                      g.bound_mutation_id),
          "AC3: effect path denies expired grant");
}

// ── AC4: #2055 surface intact ────────────────────────────────
static void ac4_schema_2055() {
    std::println("\n--- AC4: schema-2055 + grant metrics intact ---");
    reset_all();
    bump_mutation_epoch(2);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(400);
    const auto grants0 = g_capability_effect_metrics().capability_grant_epoch_bound_total.load();
    ev.grant_effect_capability(400, "ac4-g", kEffectMutate, 0);
    CHECK(g_capability_effect_metrics().capability_grant_epoch_bound_total.load() > grants0,
          "grant-epoch-bound still bumps");
    CHECK(href(cs, "schema-2055") == 2055, "schema-2055");
    CHECK(href(cs, "grant-epoch-fiber-wired") == 1, "grant-epoch-fiber-wired");
    CHECK(href(cs, "grant-epoch-bound") >= 0, "grant-epoch-bound key");
}

// ── AC5: SecurityEvent + schema-2149 ─────────────────────────
static void ac5_security_event_schema() {
    std::println("\n--- AC5: SecurityEvent joinable + schema-2149 ---");
    reset_all();
    bump_mutation_epoch(7);
    const auto me = current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(500);
    ev.grant_effect_capability(500, "ac5-g", kEffectMutate, /*prov=*/42);
    CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac5-op", 0, 500, 42),
          "effect allow");

    // SecurityEvent ring should carry mutation_id + Mutation epoch.
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    CHECK(total > 0, "security event emitted");
    bool found = false;
    const auto seq = ring.seq.load(std::memory_order_relaxed);
    const std::size_t n = std::min<std::size_t>(seq, ring.ring.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& e = ring.ring[i];
        if (e.mutation_id == 42 && (e.epoch == me || e.epoch == 1)) {
            found = true;
            std::println("  event mutation_id={} epoch={} denied={}", e.mutation_id, e.epoch,
                         e.denied);
            CHECK(!e.denied, "AC5: allow path event");
            break;
        }
    }
    CHECK(found, "AC5: SecurityEvent joinable by mutation_id / Mutation epoch");

    CHECK(href(cs, "schema-2149") == kEffectEpochUnifyIssue, "schema-2149");
    CHECK(href(cs, "effect-epoch-mutation-wired") == 1, "effect-epoch-mutation-wired");
    CHECK(href(cs, "mutation-bridge-split-total") >= 0, "split counter key");

    // make_grant_provenance still Mutation
    auto p = make_grant_provenance(0, true, 0, 0);
    CHECK(p.epoch == current_mutation_epoch() || p.epoch == 1, "make_grant still Mutation");
}

} // namespace

int run_test_effect_epoch_mutation_unify_2149() {
    std::println("=== Issue #2149: effect epoch = Mutation (not Bridge) ===");
    CHECK(kEffectEpochUnifyIssue == 2149, "issue stamp");

    ac1_effect_stamps_mutation();
    ac2_bridge_bump_no_flip();
    ac3_min_valid_fence();
    ac4_schema_2055();
    ac5_security_event_schema();

    std::println("\n=== #2149 effect epoch unify: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_effect_epoch_mutation_unify_2149();
}
#endif

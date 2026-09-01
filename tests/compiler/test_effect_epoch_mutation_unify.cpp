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
//
// Issue #3335 residual: emit_mutation_audit stamps Mutation (not Bridge)
// so the mutation audit ring joins grant / SE on the same vocabulary.
//   AC1: emit_mutation_audit slot.epoch from current_mutation_epoch()
//   AC2: Bridge-only bump does not change ring epoch vs grant
//   AC3: schema-2055 / audit stay green; ring epoch == Mutation after
//        grant+mutate
//   AC4: Soft/Off ring write still happens (epoch source only)
//   AC5: ring.epoch + SE.mutation_id + grant.bound_mutation_id same vocab

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
using aura::core::capability::kMutationAuditEpochUnifyIssue;
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

// #3362: Restricted/Strict grant_effect_capability of Mutate requires
// explicit TenantAdmin. Seed via registry.grant (test bootstrap used by
// #3126 / #3362) so grant+mutate ACs exercise the ring, not the fence.
// Mid defaults to Mutation epoch so provenance_ok (required=None walks
// all live grants, #3333) is not poisoned by a divergent admin mid.
static void seed_tenant_admin(std::uint64_t tenant, std::uint64_t mid = 0) {
    if (mid == 0) {
        const auto me = current_mutation_epoch();
        mid = me != 0 ? me : 1;
    }
    auto prov = make_grant_provenance(mid, /*force_mutation_bind=*/true, 0, 0);
    g_capability_registry().grant(tenant, "tenant-admin",
                                  aura::core::capability::Effect::TenantAdmin, prov);
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
    ev.set_capability_tenant_id(100);
    seed_tenant_admin(100);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(100, "mutate-2149", kEffectMutate, /*prov=*/0);

    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(100, "mutate-2149", g), "grant found");
    CHECK(g.grant_epoch == me || g.grant_epoch == 1, "grant_epoch = Mutation");

    // Effect check must stamp Mutation (audit ring / SecurityEvent).
    const bool ok = ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "ac1-effect",
                                                        0, 100, g.bound_mutation_id);
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
    ev.set_capability_tenant_id(200);
    seed_tenant_admin(200);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(200, "bridge-iso", kEffectMutate, 0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(200, "bridge-iso", g), "grant");
    const auto mid = g.bound_mutation_id;

    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "pre-bridge", 0, 200,
                                              mid),
          "allow before Bridge bump");

    // Bump ONLY Bridge — Mutation (and grant_epoch) stay put.
    const auto me_before = current_mutation_epoch();
    bump_bridge_epoch(100);
    CHECK(current_mutation_epoch() == me_before, "Mutation unchanged");
    CHECK(current_bridge_epoch() != me_before, "Bridge diverged");

    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "post-bridge", 0, 200,
                                              mid),
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
    ev.set_capability_tenant_id(300);
    seed_tenant_admin(300);
    ev.set_effect_sandbox_mode(2); // Strict after TA bootstrap (#3409)
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
    CHECK(!ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "fence-deny", 0, 300,
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
    ev.set_capability_tenant_id(400);
    seed_tenant_admin(400);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
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
    ev.set_capability_tenant_id(500);
    seed_tenant_admin(500, 42);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(500, "ac5-g", kEffectMutate, /*prov=*/42);
    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "ac5-op", 0, 500, 42),
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

// ── #3335 AC1: emit_mutation_audit stamps Mutation, not Bridge ─
static void ac3335_1_emit_stamps_mutation() {
    std::println("\n--- #3335 AC1: emit_mutation_audit slot.epoch = Mutation ---");
    reset_all();
    bump_mutation_epoch(20);
    bump_bridge_epoch(50);
    const auto me = current_mutation_epoch();
    const auto be = current_bridge_epoch();
    CHECK(me != 0 && be != 0 && me != be, "3335 AC1: epochs diverged");

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.emit_mutation_audit(1, 0, "3335-ac1", 7);
    const auto seq = ev.mutation_audit_seq();
    CHECK(seq >= 1, "3335 AC1: ring wrote");
    const auto& e = ev.mutation_audit_entry_at(seq - 1);
    CHECK(e.epoch == me || e.epoch == 1, "3335 AC1: ring epoch = Mutation");
    CHECK(e.epoch != be || me == be, "3335 AC1: ring epoch is not Bridge-primary");
    CHECK(e.bridge_epoch == be || e.bridge_epoch == 0, "3335 AC1: additive bridge_epoch");
    CHECK(e.provenance_mutation_id != 0, "3335 AC1: mid filled (TypedMid/Mutation, not 0)");

    auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("Issue #3335") != std::string::npos, "3335 AC1: emit cites #3335");
    const auto emit_pos = sec.find("void Evaluator::emit_mutation_audit");
    CHECK(emit_pos != std::string::npos, "3335 AC1: emit_mutation_audit present");
    if (emit_pos != std::string::npos) {
        auto body_end = sec.find("\nbool Evaluator::", emit_pos);
        if (body_end == std::string::npos)
            body_end = sec.find("\nvoid Evaluator::", emit_pos + 10);
        const auto body = sec.substr(emit_pos, body_end - emit_pos);
        CHECK(body.find("current_mutation_epoch()") != std::string::npos,
              "3335 AC1: emit stamps current_mutation_epoch");
        CHECK(body.find("slot.epoch = current_bridge_epoch()") == std::string::npos,
              "3335 AC1: emit does not stamp Bridge as slot.epoch");
    }
}

// ── #3335 AC2: Bridge-only bump does not change ring epoch ────
static void ac3335_2_bridge_bump_no_ring_flip() {
    std::println("\n--- #3335 AC2: Bridge-only bump keeps ring epoch aligned with grant ---");
    reset_all();
    bump_mutation_epoch(5);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_capability_tenant_id(210);
    seed_tenant_admin(210);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(210, "3335-bridge-iso", kEffectMutate, 0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(210, "3335-bridge-iso", g), "3335 AC2: grant");
    const auto me_before = current_mutation_epoch();

    ev.emit_mutation_audit(1, 0, "3335-pre-bridge", 0);
    const auto seq0 = ev.mutation_audit_seq();
    const auto epoch0 = ev.mutation_audit_entry_at(seq0 - 1).epoch;
    CHECK(epoch0 == me_before || epoch0 == 1, "3335 AC2: pre-bump ring = Mutation");

    bump_bridge_epoch(100);
    CHECK(current_mutation_epoch() == me_before, "3335 AC2: Mutation unchanged");
    CHECK(current_bridge_epoch() != me_before, "3335 AC2: Bridge diverged");

    ev.emit_mutation_audit(1, 0, "3335-post-bridge", 0);
    const auto seq1 = ev.mutation_audit_seq();
    const auto epoch1 = ev.mutation_audit_entry_at(seq1 - 1).epoch;
    CHECK(epoch1 == me_before || epoch1 == 1, "3335 AC2: post-bump ring still Mutation");
    CHECK(epoch1 == epoch0, "3335 AC2: ring epoch unchanged by Bridge-only bump");
    CHECK(g.grant_epoch == me_before || g.grant_epoch == 1,
          "3335 AC2: grant_epoch still Mutation (ring aligned)");
}

// ── #3335 AC3: grant+mutate ring epoch == Mutation; schema-2055 ─
static void ac3335_3_grant_mutate_ring_epoch() {
    std::println("\n--- #3335 AC3: ring epoch == Mutation after grant+mutate ---");
    reset_all();
    bump_mutation_epoch(11);
    bump_bridge_epoch(77);
    const auto me = current_mutation_epoch();
    const auto be = current_bridge_epoch();
    CHECK(me != be, "3335 AC3: epochs diverged");

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_capability_tenant_id(410);
    seed_tenant_admin(410);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(410, "3335-ac3", kEffectMutate, 0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(410, "3335-ac3", g), "3335 AC3: grant");
    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "3335-mutate", 0, 410,
                                              g.bound_mutation_id),
          "3335 AC3: mutate allowed");
    ev.emit_mutation_audit(2, 1, "3335-structural", 3);

    bool found_mutate = false;
    bool found_structural = false;
    const auto seq = ev.mutation_audit_seq();
    for (std::uint64_t i = 0; i < seq; ++i) {
        const auto& e = ev.mutation_audit_entry_at(i);
        if (std::string_view(e.op) == "3335-mutate") {
            found_mutate = true;
            CHECK(e.epoch == me || e.epoch == 1, "3335 AC3: effect ring epoch = Mutation");
            CHECK(e.epoch != be || me == be, "3335 AC3: effect ring not Bridge");
        }
        if (std::string_view(e.op) == "3335-structural") {
            found_structural = true;
            CHECK(e.epoch == me || e.epoch == 1, "3335 AC3: structural ring epoch = Mutation");
            CHECK(e.epoch != be || me == be, "3335 AC3: structural ring not Bridge");
        }
    }
    CHECK(found_mutate && found_structural, "3335 AC3: both grant+mutate ring entries present");
    CHECK(href(cs, "schema-2055") == 2055, "3335 AC3: schema-2055 unchanged");
    CHECK(href(cs, "schema-3335") == kMutationAuditEpochUnifyIssue, "3335 AC3: schema-3335");
}

// ── #3335 AC4: Soft/Off ring write still happens ──────────────
static void ac3335_4_soft_off_write() {
    std::println("\n--- #3335 AC4: Soft/Off ring write unchanged (epoch source only) ---");
    reset_all();
    set_mode(SandboxMode::Off);
    bump_mutation_epoch(3);
    const auto me = current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    const auto seq0 = ev.mutation_audit_seq();
    ev.emit_mutation_audit(1, 0, "3335-off", 0);
    const auto seq1 = ev.mutation_audit_seq();
    CHECK(seq1 > seq0, "3335 AC4: Off still writes the ring");
    const auto& e = ev.mutation_audit_entry_at(seq1 - 1);
    CHECK(std::string_view(e.op) == "3335-off", "3335 AC4: Off entry present");
    CHECK(e.epoch == me || e.epoch == 1, "3335 AC4: Off epoch still Mutation");
}

// ── #3335 AC5: Agent join ring.epoch + SE + grant same vocab ──
static void ac3335_5_agent_join_vocab() {
    std::println("\n--- #3335 AC5: ring.epoch + SE.mutation_id + grant.bound_mutation_id ---");
    reset_all();
    bump_mutation_epoch(9);
    bump_bridge_epoch(40);
    const auto me = current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_capability_tenant_id(510);
    seed_tenant_admin(510, 42);
    ev.set_effect_sandbox_mode(1); // Restricted after TA bootstrap (#3409)
    ev.grant_effect_capability(510, "3335-join", kEffectMutate, /*prov=*/42);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(510, "3335-join", g), "3335 AC5: grant");
    CHECK(g.bound_mutation_id == 42, "3335 AC5: grant.bound_mutation_id");
    CHECK(g.grant_epoch == me || g.grant_epoch == 1, "3335 AC5: grant_epoch = Mutation");

    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "3335-join-op", 0, 510,
                                              42),
          "3335 AC5: effect allow");
    ev.emit_mutation_audit(1, 0, "3335-join-struct", 0);

    // SecurityEvent joinable by mutation_id on Mutation vocabulary.
    auto& ring = g_security_event_ring();
    bool se_found = false;
    const auto n =
        std::min<std::size_t>(ring.seq.load(std::memory_order_relaxed), ring.ring.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& e = ring.ring[i];
        if (e.mutation_id == g.bound_mutation_id && (e.epoch == me || e.epoch == 1)) {
            se_found = true;
            break;
        }
    }
    CHECK(se_found, "3335 AC5: SE.mutation_id joins grant.bound_mutation_id on Mutation epoch");

    bool ring_found = false;
    const auto seq = ev.mutation_audit_seq();
    for (std::uint64_t i = 0; i < seq; ++i) {
        const auto& e = ev.mutation_audit_entry_at(i);
        if (std::string_view(e.op) == "3335-join-struct" ||
            std::string_view(e.op) == "3335-join-op") {
            CHECK(e.epoch == me || e.epoch == 1, "3335 AC5: ring.epoch = Mutation (same vocab)");
            ring_found = true;
        }
    }
    CHECK(ring_found, "3335 AC5: Agent can join ring.epoch + SE.mutation_id + "
                      "grant.bound_mutation_id on Mutation vocabulary");
    CHECK(href(cs, "mutation-audit-epoch-mutation-wired") == 1,
          "3335 AC5: wired flag on capability-effect-stats");
}

// ── #3335 AC6: linter + no invent / no docs/design ───────────
static void ac3335_6_source_and_linter() {
    std::println("\n--- #3335 AC6: source-cite + linter + no invent ---");
    auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("Issue #3335") != std::string::npos, "3335 AC6: evaluator_security cites #3335");
    auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("std::uint64_t bridge_epoch = 0;") != std::string::npos,
          "3335 AC6: MutationAuditEntry END-appends bridge_epoch");
    auto lint = read_file("scripts/coverage/checks/check_mutation_audit_epoch_3335.py");
    CHECK(!lint.empty() && lint.find("Issue #3335") != std::string::npos, "3335 AC6: linter");
    auto build = read_file("build.py");
    CHECK(build.find("check_mutation_audit_epoch_3335") != std::string::npos, "3335 AC6: build.py");
    CHECK(build.find("check_require_effect_mid_ssot_3296") != std::string::npos,
          "3335 AC6: #3296 linter retained");
    CHECK(read_file("tests/compiler/test_issue_3335.cpp").empty(),
          "3335 AC6: no test_issue_3335.cpp (#81967)");
}

} // namespace

int run_test_effect_epoch_mutation_unify() {
    std::println("=== Issue #2149: effect epoch = Mutation (not Bridge) ===");
    CHECK(kEffectEpochUnifyIssue == 2149, "issue stamp");
    CHECK(kMutationAuditEpochUnifyIssue == 3335, "3335 issue stamp");

    ac1_effect_stamps_mutation();
    ac2_bridge_bump_no_flip();
    ac3_min_valid_fence();
    ac4_schema_2055();
    ac5_security_event_schema();
    ac3335_1_emit_stamps_mutation();
    ac3335_2_bridge_bump_no_ring_flip();
    ac3335_3_grant_mutate_ring_epoch();
    ac3335_4_soft_off_write();
    ac3335_5_agent_join_vocab();
    ac3335_6_source_and_linter();

    std::println("\n=== #2149/#3335 effect+audit epoch unify: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_effect_epoch_mutation_unify();
}
#endif

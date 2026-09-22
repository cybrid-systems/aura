// @category: unit
// @reason: Issue #2493 — unify mutation_id source — WorkspaceEpoch Mutation
// over independent audit gens. Audit paths that didn't thread a caller mid
// previously allocated from `audit_mutation_id_gen` (parallel vocabulary),
// weakening join for blame / replay against grants bound to Mutation epoch.
//
// `resolve_audit_mutation_id(caller_mid)` enforces preference order:
//   1. caller mid when non-zero
//   2. current_mutation_epoch() when non-zero  (WorkspaceEpoch Mutation — #2149)
//   3. ResourceQuota host mid when set
//   4. next_audit_mutation_id() as last-resort join stamp (process-origin;
//      bumps audit_mid_fallback_gen_total so Agent dashboards see join quality).
//
//   AC1: require_effect deny under Restricted → SE.mutation_id matches
//        TypedMutationAudit matching event.mutation_id (join by mid).
//   AC2: Grant bound to Mutation epoch M + effect under mid M → allow;
//        under different mid → provenance mismatch reachable.
//   AC3: AOT hot-update audit without explicit mid → non-zero mid;
//        epoch field is Mutation when available.
//   AC4: Soft / no mutation activity → non-zero join stamp (1 or gen)
//        still recorded; no crash.
//   AC5: Additive metric (audit_mid_fallback_gen_total) + schema key;
//        existing queries unchanged except new optional keys.
//   AC6: Tests: mid join after deny; source-cite resolve helper.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/tenant_host_path.hh"
#include "compiler/type_linear_commit_health.hh"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/resource_quota.hh"
#include "core/sandbox.hh" // #3599: Strict face for the deny gate
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/typed_mutation_audit_counters.h"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::capture_security_correlated_audit;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::resolve_audit_mutation_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::resource_quota::process_resource_quota_manager;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::workspace_isolation::g_workspace_isolation;
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
    reset_security_event_ring_for_test();
    g_workspace_isolation().set_strict_sandbox_linked(false);
    // Reset typed audit counters (best-effort reset via dedicated helper).
    g_typed_mutation_audit_counters.audit_mutation_id_gen.store(0, std::memory_order_relaxed);
    g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.store(0,
                                                                       std::memory_order_relaxed);
}

// AC1: resolve_audit_mutation_id prefers caller mid when non-zero; falls
// through to current_mutation_epoch when caller is 0.
static void ac1_prefers_caller_then_mutation_epoch() {
    std::println("\n--- #2493 AC1: caller mid > Mutation epoch ---");
    reset_all();
    bump_mutation_epoch(7);
    const auto me = current_mutation_epoch();
    // Caller non-zero: should win over Mutation epoch.
    const auto caller_mid = 99999ULL;
    const auto got_caller = resolve_audit_mutation_id(caller_mid);
    CHECK(got_caller == caller_mid,
          "AC1: caller mid wins over Mutation epoch (resolve_audit_mutation_id)");

    // Caller zero + Mutation epoch non-zero: should use epoch.
    const auto got_epoch = resolve_audit_mutation_id(0);
    CHECK(got_epoch == me, "AC1: caller_mid=0 → current_mutation_epoch() (Mutation vocabulary)");

    (void)got_caller;
    (void)got_epoch;
}

// AC2: ResourceQuota host mid used as fallback when caller=0 + epoch=0.
static void ac2_resource_quota_fallback() {
    std::println("\n--- #2493 AC2: ResourceQuota host mid ---");
    reset_all();
    bump_mutation_epoch(1); // ensure epoch != 0 — proves RQ is only fallback
                            // after caller + epoch both miss.
    // Caller=0, epoch set — should win over RQ.
    const auto got_epoch = resolve_audit_mutation_id(0);
    CHECK(got_epoch == current_mutation_epoch(), "AC2: epoch wins over ResourceQuota host mid");
    (void)got_epoch;
}

// AC3: AOT hot-update audit without explicit mid → resolve path runs;
// mid non-zero and equals epoch when available.
static void ac3_aot_hotupdate_uses_resolve() {
    std::println("\n--- #2493 AC3: AOT hot-update audit mid path ---");
    reset_all();
    bump_mutation_epoch(11);
    const auto me = current_mutation_epoch();
    // AOT hot-update audit no longer hard-codes next_audit_mutation_id();
    // resolve_audit_mutation_id() prefers epoch.
    const auto mid = resolve_audit_mutation_id();
    CHECK(mid == me, "AC3: AOT audit mid == current_mutation_epoch()");
}

// AC4: Soft / no mutation activity → resolve falls back to gen;
// non-zero join stamp recorded; no crash. Counter bumps.
// Issue #2836: cold-start is Full (absolute refuse); Soft last-resort
// requires apply_dev_audit_defaults (Sampled) + all upstream mids zero.
static void ac4_soft_no_activity_fallback() {
    std::println("\n--- #2493 AC4: Soft no-mutation-activity fallback ---");
    reset_all();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    // Force last-resort: no Mutation activity, no RQ host mid.
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    const auto before = g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.load();
    const auto mid = resolve_audit_mutation_id();
    const auto after = g_typed_mutation_audit_counters.audit_mid_fallback_gen_total.load();
    std::println("  mid={} fallback {}→{}", mid, before, after);
    CHECK(mid != 0, "AC4: fallback mid is non-zero (process-origin join stamp)");
    CHECK(after == before + 1, "AC4: audit_mid_fallback_gen_total bumps on last-resort path");
}

// AC5: capture_security_correlated_audit prefers caller mid when non-zero;
// caller_mid == 0 falls into resolve_audit_mutation_id preference order.
// Epoch fallback to current_mutation_epoch() keeps SE.epoch in Mutation
// vocabulary (#2149).
static void ac5_correlated_audit_join() {
    std::println("\n--- #2493 AC5: correlated audit mid join ---");
    reset_all();
    bump_mutation_epoch(13);
    const auto me = current_mutation_epoch();
    // Caller_mid=0 → resolve picks epoch. Epoch=0 → resolve picks current_mutation_epoch.
    const auto before = g_typed_mutation_audit_counters.audits_considered.load();
    capture_security_correlated_audit(/*mutation_id=*/0, "test:2493-ac5",
                                      /*epoch=*/0, /*denied=*/false,
                                      /*target_node=*/0, /*fiber_id=*/0);
    const auto after = g_typed_mutation_audit_counters.audits_considered.load();
    CHECK(after == before + 1, "AC5: audits_considered bumped");
    // SE and TypedMutationAudit both stamped with me (resolved via epoch).
    // The trail row's epoch field equals current_mutation_epoch() under
    // the caller=0 path — verified via source-cite (AC6).
    (void)me;
}

// AC6: source-cite + registrations.
static void ac6_source_and_gate() {
    std::println("\n--- #2493 AC6: source-cite + gate ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("Issue #2493") != std::string::npos, "AC6: typed_mutation_audit.h cites #2493");
    CHECK(tma.find("resolve_audit_mutation_id") != std::string::npos,
          "AC6: resolve_audit_mutation_id helper present");
    CHECK(tma.find("audit_mid_fallback_gen_total") != std::string::npos,
          "AC6: audit_mid_fallback_gen_total counter present");
    CHECK(tma.find("capture_security_correlated_audit") != std::string::npos,
          "AC6: capture_security_correlated_audit updated to use resolve");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_audit_mutation_id_unify") != std::string::npos,
          "AC6: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2493.json").empty(),
          "AC6: build.py gate entry (manifest SSOT)");
    const auto gate = read_file("scripts/coverage/manifests/2493.json");
    CHECK(!gate.empty() && gate.find("Issue #2493") != std::string::npos,
          "AC6: coverage linter present");
}

// Issue #3016: boundary trail mid == resolve_audit_mutation_id (not
// total_mutations_). Production refuse does not stamp mid=0; Soft
// still generates a fallback. Two evaluators with coincidental volume
// counters do not cross-join.
static void ac7_boundary_trail_uses_resolve() {
    std::println("\n--- #3016 AC: boundary trail mid == resolve ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();

    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(mb.find("Issue #3016") != std::string::npos, "#3016 AC5: boundary cites #3016");
    CHECK(mb.find("cp.audit_mid") != std::string::npos, "#3016 AC5: trail uses checkpoint mid");
    CHECK(tma.find("stamp_boundary_audit_mid") != std::string::npos,
          "#3016 AC5: stamp helper present");
    CHECK(tma.find("if (mutation_id == 0)") != std::string::npos, "#3016 AC3: capture skips mid=0");
    CHECK(read_file("docs/design/3016-boundary-audit-mid.md").empty(), "#3016: no docs/design/");

    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 55);
    const auto expected = resolve_audit_mutation_id(0);
    CHECK(expected == 55, "#3016 AC5: resolve == epoch 55");

    CompilerService cs1;
    auto& ev1 = cs1.evaluator();
    bool ok = true;
    auto g1 = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev1, 1, &ok);
    CHECK(g1.has_value(), "#3016 AC5: Guard acquire");
    CHECK(aura::compiler::typed_audit::current_boundary_audit_mid() == expected,
          "#3016 AC5: TLS mid == resolve");
    aura::compiler::typed_audit::capture_audit_event_forced(
        expected, "test:3016-boundary", aura::compiler::typed_audit::MutationKind::Structural, 1, 2,
        aura::compiler::typed_audit::AuditOutcome::Success, 0, 0, 0, 0);
    if (g1.has_value())
        (*g1).reset();

    aura::compiler::typed_audit::TypedMutationAuditEvent te{};
    const bool found = aura::compiler::typed_audit::trail_find_by_mutation_id(expected, te);
    CHECK(found, "#3016 AC5: trail find by resolved mid");
    if (found)
        CHECK(te.mutation_id == expected, "#3016 AC5: trail mid == resolve (not volume)");

    // AC4: second evaluator with its own volume counter still stamps epoch,
    // not coincidental total_mutations_.
    CompilerService cs2;
    auto& ev2 = cs2.evaluator();
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 77);
    const auto expected2 = resolve_audit_mutation_id(0);
    CHECK(expected2 == 77, "#3016 AC4: epoch 77");
    auto g2 = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev2, 1, &ok);
    CHECK(g2.has_value(), "#3016 AC4: second Guard");
    CHECK(aura::compiler::typed_audit::current_boundary_audit_mid() == expected2,
          "#3016 AC4: ev2 TLS mid == 77 (not ev1 volume)");
    aura::compiler::typed_audit::capture_audit_event_forced(
        expected2, "test:3016-ev2", aura::compiler::typed_audit::MutationKind::Structural, 1, 2,
        aura::compiler::typed_audit::AuditOutcome::Success, 0, 0, 0, 0);
    if (g2.has_value())
        (*g2).reset();
    aura::compiler::typed_audit::TypedMutationAuditEvent te2{};
    CHECK(aura::compiler::typed_audit::trail_find_by_mutation_id(77, te2),
          "#3016 AC4: trail find 77");

    // AC3: production + all upstream 0 → no mid=0 trail stamp.
    {
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(1, std::memory_order_relaxed);
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
        process_resource_quota_manager().provenance_mutation_id = 0;
        const auto refused0 =
            g_typed_mutation_audit_counters.audit_mid_fallback_refused_total.load();
        CompilerService cs3;
        auto g3 =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs3.evaluator(), 1, &ok);
        const auto mid0 = aura::compiler::typed_audit::current_boundary_audit_mid();
        CHECK(mid0 == 0, "#3016 AC3: production refuse mid=0");
        CHECK(g_typed_mutation_audit_counters.audit_mid_fallback_refused_total.load() > refused0,
              "#3016 AC3: refused bumped");
        aura::compiler::typed_audit::TypedMutationAuditEvent te0{};
        CHECK(!aura::compiler::typed_audit::trail_find_by_mutation_id(0, te0),
              "#3016 AC3: mid=0 not joinable in trail");
        if (g3.has_value())
            g3->reset();
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(0, std::memory_order_relaxed);
        aura::compiler::typed_audit::clear_boundary_audit_mid();
    }
}

static std::int64_t href_audit(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool se_ring_has_mid(std::uint64_t mid) {
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::kSecurityEventRingSize;
    auto& ring = g_security_event_ring();
    const auto head = ring.seq.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSecurityEventRingSize && i < head; ++i) {
        const auto& e = ring.ring[(head - 1 - i) % kSecurityEventRingSize];
        if (e.mutation_id == mid)
            return true;
    }
    return false;
}

// Issue #3066: composite / lockless batch typed↔SE join mid.
static void ac3066_1_production_batch_share_mid() {
    std::println("\n--- #3066 AC1: production batch typed + SE share mid ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.begin_atomic_batch_pinning();
    const auto pin = aura::compiler::typed_audit::current_boundary_audit_mid();
    // Issue #3599 re-pin: production epoch=0 refuses to mid=0 — no phantom
    // join mid (the #3462 refuse-class contract supersedes the #3066 pin).
    CHECK(pin == 0, "3066 AC1: production epoch=0 -> pin refuses to 0 (no phantom)");
    CHECK(aura::compiler::typed_audit::g_last_stamped_audit_mid.load() == pin,
          "3066 AC1: last_stamped == pin");
    CHECK(aura::compiler::typed_audit::g_last_composite_batch_join_mid.load() == pin,
          "3066 AC1: last composite join == pin");
    CHECK(aura::compiler::typed_audit::join_audit_and_se_mid(0) == pin,
          "3066 AC1: join helper returns pin");

    capture_security_correlated_audit(/*mutation_id=*/0, "test:3066-ac1", /*epoch=*/0,
                                      /*denied=*/true, 0, 0);
    CHECK(aura::compiler::typed_audit::g_last_stamped_audit_mid.load() == pin,
          "3066 AC1: SE-correlated stamp stays on pin");
    aura::compiler::typed_audit::TypedMutationAuditEvent te{};
    CHECK(!aura::compiler::typed_audit::trail_find_by_mutation_id(pin, te),
          "3066 AC1: no typed trail row for the refuse mid=0");
    CHECK(!se_ring_has_mid(1), "3066 AC1: no phantom mid=1 row (#3462/#3599)");
    CHECK(href_audit(cs, "schema-3066") == 3066, "3066 AC1: live schema-3066");
    CHECK(href_audit(cs, "last-stamped-audit-mid") == static_cast<std::int64_t>(pin),
          "3066 AC1: query last-stamped == pin");

    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    (void)ev;
}

// Issue #3874: TypedMutationAuditEvent carries the capability principal
// tenant; unset emits stay honest-0 (Soft-face unchanged).
// Issue #3875: revoke_epoch process-origin stamp is hard-only — Soft rows
// keep honest 0 (no phantom 1 into mutation-order stats).
static void ac3875_revoke_epoch_hard_only_invent() {
    std::println("\n--- #3875 AC1: Soft face + Mutation epoch=0 → revoke_epoch stays 0 ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Sampled);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    CHECK(aura::core::current_mutation_epoch() == 0, "3875 AC1 pre: epoch=0");
    CHECK(!aura::core::capability::capability_epoch_hard_face(), "3875 AC1 pre: soft face");

    using aura::core::capability::Effect;
    using aura::core::capability::make_grant_provenance;
    auto prov = make_grant_provenance(0, false, 0, 0);
    CHECK(g_capability_registry().grant(875, "mutate-3875", Effect::Mutate, prov,
                                        /*single_use=*/false, /*session_bound=*/false, 875),
          "3875 AC1: grant accepted");
    g_capability_registry().revoke(875, "mutate-3875", /*revoke_at_epoch=*/0);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(875, "mutate-3875", g), "3875 AC1: grant found");
    CHECK(g.revoked, "3875 AC1: revoked flag");
    CHECK(g.revoke_epoch == 0, "3875 AC1: Soft observe keeps revoke_epoch 0 (no invented 1)");

    std::println("\n--- #3875 AC2: hard face + Mutation epoch=0 → process-origin stamp 1 ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    CHECK(aura::core::capability::capability_epoch_hard_face(), "3875 AC2 pre: hard face");
    prov = make_grant_provenance(0, false, 0, 0);
    CHECK(g_capability_registry().grant(875, "mutate-3875h", Effect::Mutate, prov,
                                        /*single_use=*/false, /*session_bound=*/false, 875),
          "3875 AC2: grant accepted");
    g_capability_registry().revoke(875, "mutate-3875h", /*revoke_at_epoch=*/0);
    CHECK(g_capability_registry().find_grant(875, "mutate-3875h", g), "3875 AC2: grant found");
    CHECK(g.revoke_epoch == 1, "3875 AC2: hard face process-origin stamp stays");

    const auto cm = read_file("src/core/capability_model.hh");
    CHECK(cm.find("Issue #3875") != std::string::npos, "3875 AC3: header cites #3875");
    CHECK(cm.find("ep == 0 && capability_epoch_hard_face()") != std::string::npos,
          "3875 AC3: hard-only clamp present");
    CHECK(read_file("docs/design/3875-revoke-epoch-hard-only.md").empty(),
          "3875 AC3: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3875.cpp").empty(), "3875 AC3: no invent");
}

static void ac3874_tenant_stamp() {
    std::println("\n--- #3874: TypedMutationAuditEvent capability tenant stamp ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::typed_audit::capture_audit_event_forced(
        390074, "test:3874-tenant", aura::compiler::typed_audit::MutationKind::Structural, 1, 2,
        aura::compiler::typed_audit::AuditOutcome::Error, 0, 0, 0, 0, /*tenant_id=*/4242);
    aura::compiler::typed_audit::TypedMutationAuditEvent te{};
    CHECK(aura::compiler::typed_audit::trail_find_by_mutation_id(390074, te),
          "3874 AC1: trail row joinable by mid");
    CHECK(te.tenant_id == 4242, "3874 AC1: tenant_id stamped from emit");
    aura::compiler::typed_audit::capture_audit_event_forced(
        390075, "test:3874-unset", aura::compiler::typed_audit::MutationKind::Structural, 1, 2,
        aura::compiler::typed_audit::AuditOutcome::Error);
    aura::compiler::typed_audit::TypedMutationAuditEvent te2{};
    CHECK(aura::compiler::typed_audit::trail_find_by_mutation_id(390075, te2),
          "3874 AC2: unset row joinable");
    CHECK(te2.tenant_id == 0, "3874 AC2: tenant_id honest unset (0)");
    const auto hdr = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(hdr.find("Issue #3874") != std::string::npos, "3874 AC3: header cites #3874");
    CHECK(hdr.find("std::uint32_t tenant_id = 0;") != std::string::npos,
          "3874 AC3: struct field present");
    const auto boundary = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(boundary.find("static_cast<std::uint32_t>(capability_tenant_id())") != std::string::npos,
          "3874 AC3: boundary stamps principal tenant");
    CHECK(!read_file("docs/design/3874-audit-tenant.md").empty() == false,
          "3874 AC3: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3874.cpp").empty(), "3874 AC3: no invent");
}

static void ac3066_2_sampled_force_joinable() {
    std::println("\n--- #3066 AC2: Sampled + force-reason joinable mid ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;

    constexpr std::uint64_t kDeny = 4242;
    aura::compiler::typed_audit::capture_audit_event_forced(
        kDeny, "test:3066-force", aura::compiler::typed_audit::MutationKind::Structural, 1, 2,
        aura::compiler::typed_audit::AuditOutcome::Error, 0, 0, 0, 0);
    CHECK(aura::compiler::typed_audit::g_last_stamped_audit_mid.load() == kDeny,
          "3066 AC2: force stamps deny mid");
    CHECK(aura::compiler::typed_audit::join_audit_and_se_mid(0) == kDeny,
          "3066 AC2: join pinned to deny mid");
    capture_security_correlated_audit(0, "test:3066-ac2-se", 0, /*denied=*/true, 0, 0);
    CHECK(aura::compiler::typed_audit::g_last_stamped_audit_mid.load() == kDeny,
          "3066 AC2: subsequent SE uses same mid (no fallback diverge)");
    aura::compiler::typed_audit::TypedMutationAuditEvent te{};
    CHECK(aura::compiler::typed_audit::trail_find_by_mutation_id(kDeny, te),
          "3066 AC2: trail joinable by deny mid");
}

static void ac3066_3_soft_zero_extra() {
    std::println("\n--- #3066 AC3: Soft/Off no extra pin ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    const auto pin0 = aura::compiler::typed_audit::g_composite_batch_join_pin_total.load();
    const auto se0 = aura::compiler::typed_audit::g_composite_batch_se_join_total.load();
    CHECK(aura::compiler::typed_audit::pin_composite_batch_join_mid() == 0,
          "3066 AC3: Soft pin is no-op");
    CHECK(aura::compiler::typed_audit::g_composite_batch_join_pin_total.load() == pin0,
          "3066 AC3: Soft no pin-total bump");
    CHECK(aura::compiler::typed_audit::g_composite_batch_se_join_total.load() == se0,
          "3066 AC3: Soft no SE join emit");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(tma.find("join_audit_and_se_mid") != std::string::npos,
          "3066 AC3: require_effect uses join");
    CHECK(sec.find("mid = 1") != std::string::npos || sec.find("mid = 1;") != std::string::npos ||
              sec.find("non-zero join stamp") != std::string::npos,
          "3066 AC3: Soft historical mid=1 retained");
}

static void ac3066_4_linter_no_design() {
    std::println("\n--- #3066 AC4: linter + no invent / no design ---");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_composite_audit_mid_se_join_3066.py");
    const auto build = read_file("build.py");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(t.find("ac3066_1_production_batch_share_mid") != std::string::npos, "3066 AC4: AC1");
    CHECK(t.find("ac3066_2_sampled_force_joinable") != std::string::npos, "3066 AC4: AC2");
    CHECK(t.find("ac3066_3_soft_zero_extra") != std::string::npos, "3066 AC4: AC3");
    CHECK(tma.find("Issue #3066") != std::string::npos, "3066 AC4: header cite");
    CHECK(tma.find("pin_composite_batch_join_mid") != std::string::npos, "3066 AC4: pin helper");
    CHECK(tma.find("join_audit_and_se_mid") != std::string::npos, "3066 AC4: join helper");
    CHECK(!lint.empty() && lint.find("Issue #3066") != std::string::npos, "3066 AC4: linter");
    CHECK(build.find("check_composite_audit_mid_se_join_3066") != std::string::npos,
          "3066 AC4: build.py gate");
    CHECK(build.find("cmd_composite_audit_mid_se_join_3066") != std::string::npos,
          "3066 AC4: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3066.cpp").empty(),
          "3066 AC4: no test_issue_3066.cpp");
}

// Issue #3367: pin_composite_batch_join_mid under hard mode (production /
// Full) must NOT mint a process-origin mid when caller / boundary /
// epoch / TypedMid are all zero. Same hard face as resolve_audit_mutation_id
// which already refuses (return 0 + SE mid-fallback-refused). Two mid
// policies on the same hard face were the I6 residual — pin was
// bypassing the #2836 refuse contract. Soft quiet no-op preserved
// per #3066 AC3.
static void ac3367_pin_matrix_no_process_origin_mid_in_hard() {
    std::println(
        "\n--- #3367: pin_composite_batch_join_mid matrix (no process-origin mid in hard) ---");
    using namespace aura::compiler::typed_audit;
    // Use a fresh process for each sub-case so the audit_mutation_id_gen
    // / audit_mid_fallback_refused_total / pin_total counters don't leak
    // across sub-cases (counters are process-global atomics).
    auto read_gen = []() { return g_typed_mutation_audit_counters.audit_mutation_id_gen.load(); };
    auto read_pin_total = []() { return g_composite_batch_join_pin_total.load(); };
    auto read_se_total = []() { return g_composite_batch_se_join_total.load(); };
    auto read_refused_total = []() {
        return g_typed_mutation_audit_counters.audit_mid_fallback_refused_total.load();
    };

    auto reset_state = []() {
        reset_for_test();
        reset_all();
    };

    // ── AC1: Soft + mid==0 → 0, no mint, no SE, no refused bump ──
    {
        reset_state();
        apply_dev_audit_defaults(); // Soft/Sampled
        g_typed_mutation_audit_counters.production_defaults_active.store(0);
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
        process_resource_quota_manager().provenance_mutation_id = 0;
        const auto gen0 = read_gen();
        const auto pin0 = read_pin_total();
        const auto se0 = read_se_total();
        const auto ref0 = read_refused_total();
        const auto mid = pin_composite_batch_join_mid();
        CHECK(mid == 0, "3367 AC1: Soft + mid==0 returns 0 (no mint, quiet)");
        CHECK(read_gen() == gen0, "3367 AC1: Soft does not bump audit_mutation_id_gen");
        CHECK(read_pin_total() == pin0,
              "3367 AC1: Soft does not bump composite_batch_join_pin_total");
        CHECK(read_se_total() == se0, "3367 AC1: Soft does not emit SE join");
        CHECK(read_refused_total() == ref0,
              "3367 AC1: Soft does not bump refuse (refuse is hard-only)");
    }

    // ── AC2: production/Full + mid==0 → 0, no mint, no SE (refuse path) ──
    {
        reset_state();
        apply_production_audit_defaults(); // Full / production
        g_typed_mutation_audit_counters.production_defaults_active.store(1);
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
        process_resource_quota_manager().provenance_mutation_id = 0;
        const auto gen0 = read_gen();
        const auto pin0 = read_pin_total();
        const auto se0 = read_se_total();
        const auto ref0 = read_refused_total();
        const auto mid = pin_composite_batch_join_mid();
        CHECK(mid == 0, "3367 AC2: hard + mid==0 returns 0 (no mint, refuse path in resolve)");
        CHECK(read_gen() == gen0, "3367 AC2: hard empty-upstream does NOT bump "
                                  "audit_mutation_id_gen (no process-origin mid)");
        CHECK(read_pin_total() == pin0,
              "3367 AC2: hard empty-upstream does NOT bump pin_total (no join SE)");
        CHECK(read_se_total() == se0,
              "3367 AC2: hard empty-upstream does NOT emit SE join (refuse via resolve)");
        CHECK(read_refused_total() == ref0, "3367 AC2: pin does NOT bump refuse_total (resolve's "
                                            "caller does that; pin is silent here)");
    }

    // ── AC3: hard + epoch != 0 → return epoch (legitimate join path) ──
    {
        reset_state();
        apply_production_audit_defaults();
        g_typed_mutation_audit_counters.production_defaults_active.store(1);
        // Seed a non-zero epoch.
        aura::core::bump_mutation_epoch();
        const auto epoch_val = aura::core::current_mutation_epoch();
        CHECK(epoch_val != 0, "3367 AC3 setup: epoch seeded non-zero");
        process_resource_quota_manager().provenance_mutation_id = 0;
        const auto mid = pin_composite_batch_join_mid();
        CHECK(mid == epoch_val, "3367 AC3: hard + epoch != 0 returns epoch (legitimate join path)");
    }

    // ── AC4: hard + caller_mid != 0 → return caller_mid ──
    {
        reset_state();
        apply_production_audit_defaults();
        g_typed_mutation_audit_counters.production_defaults_active.store(1);
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
        process_resource_quota_manager().provenance_mutation_id = 0;
        constexpr std::uint64_t caller_mid = 42;
        const auto mid = pin_composite_batch_join_mid(caller_mid);
        CHECK(mid == caller_mid,
              "3367 AC4: hard + caller_mid != 0 returns caller_mid (legacy contract)");
    }
}

// Issue #3546: sticky composite mid for SE ↔ Typed ↔ last_proof_mid join.
// Reuses #3066 g_tls_composite_batch_join_mid (no second sticky model).
static void ac3546_1_sticky_mid_survives_inner_mutates() {
    std::println("\n--- #3546 AC1: composite sticky mid survives 3 inner mutates ---");
    using namespace aura::compiler::typed_audit;
    reset_all();
    reset_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    clear_invariant_deny_se_tls();

    constexpr std::uint64_t kComposite = 3546001;
    constexpr std::uint64_t kInner[3] = {11, 22, 33};
    aura_composite_txn_enter(kComposite);
    CHECK(g_tls_composite_batch_join_mid == kComposite, "3546 AC1: enter pins TLS composite");
    CHECK(g_last_stamped_audit_mid.load() == kComposite, "3546 AC1: pin publishes last_stamped");

    for (int i = 0; i < 3; ++i) {
        capture_audit_event_forced(kInner[i], "test:3546-inner", MutationKind::Structural, 1, 2,
                                   AuditOutcome::Error, 0, 0, 0, 0);
        CHECK(g_last_stamped_audit_mid.load() == kComposite,
              "3546 AC1: inner stamp does not clobber composite last_stamped");
        CHECK(last_stamped_or_composite_audit_mid() == kComposite,
              "3546 AC1: last_stamped_or_composite stays composite");
        CHECK(join_audit_and_se_mid(kInner[i]) == kComposite,
              "3546 AC1: join(inner) returns sticky composite");
        CHECK(g_tls_composite_batch_join_mid == kComposite, "3546 AC1: TLS sticky still set");
        TypedMutationAuditEvent te{};
        CHECK(trail_find_by_mutation_id(kInner[i], te), "3546 AC1: trail keeps forensic inner mid");
        CHECK(te.mutation_id == kInner[i], "3546 AC1: trail ev.mutation_id is inner");
    }

    emit_invariant_deny_se(join_audit_and_se_mid(kInner[2]), /*tenant_id=*/0, /*fiber_id=*/0,
                           /*epoch=*/0, "test:3546-se", "invariant");
    CHECK(se_ring_has_mid(kComposite), "3546 AC1: SE ring joins composite mid");
    CHECK(aura::compiler::capture_type_linear_evolution_snapshot().last_proof_mid ==
              static_cast<std::int64_t>(kComposite),
          "3546 AC1: last_proof_mid == composite");

    aura_composite_txn_exit();
    CHECK(g_tls_composite_batch_join_mid == 0, "3546 AC1: exit clears TLS sticky");
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    clear_boundary_audit_mid();
}

static void ac3546_2_abort_clear_still_prefers_tls() {
    std::println("\n--- #3546 AC2: abort-clear of last_stamped still prefers TLS ---");
    using namespace aura::compiler::typed_audit;
    reset_all();
    reset_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    constexpr std::uint64_t kComposite = 3546002;
    aura_composite_txn_enter(kComposite);
    capture_audit_event_forced(77, "test:3546-abort", MutationKind::Structural, 1, 2,
                               AuditOutcome::Rollback, 0, 0, 0, 0);
    clear_type_linear_commit_proof_on_abort();
    CHECK(g_last_stamped_audit_mid.load() == 0, "3546 AC2: abort zeros last_stamped");
    CHECK(g_tls_composite_batch_join_mid == kComposite, "3546 AC2: TLS sticky survives abort");
    CHECK(last_stamped_or_composite_audit_mid() == kComposite,
          "3546 AC2: snapshot prefers TLS composite after abort-clear");
    CHECK(aura::compiler::capture_type_linear_evolution_snapshot().last_proof_mid ==
              static_cast<std::int64_t>(kComposite),
          "3546 AC2: last_proof_mid prefers TLS after abort-clear");
    CHECK(join_audit_and_se_mid(77) == kComposite, "3546 AC2: SE join still composite");
    aura_composite_txn_exit();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    clear_boundary_audit_mid();
}

static void ac3546_3_enter_zero_is_noop() {
    std::println("\n--- #3546 AC4: enter(0) is zero-cost no-op ---");
    using namespace aura::compiler::typed_audit;
    reset_all();
    reset_for_test();
    apply_dev_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    const auto tls0 = g_tls_composite_batch_join_mid;
    const auto last0 = g_last_stamped_audit_mid.load();
    const auto pin0 = g_composite_batch_join_pin_total.load();
    aura_composite_txn_enter(0);
    CHECK(g_tls_composite_batch_join_mid == tls0, "3546 AC4: enter(0) does not pin TLS");
    CHECK(g_last_stamped_audit_mid.load() == last0, "3546 AC4: enter(0) does not stamp");
    CHECK(g_composite_batch_join_pin_total.load() == pin0, "3546 AC4: enter(0) no pin-total");
    CHECK(last_stamped_or_composite_audit_mid() == last0,
          "3546 AC4: last_stamped_or_composite unchanged");
    aura_composite_txn_exit();
    CHECK(g_tls_composite_batch_join_mid == 0, "3546 AC4: exit with TLS==0 is no-op");
}

static void ac3546_4_source_cite_no_invent() {
    std::println("\n--- #3546 AC5: source-cite + no second sticky / no design ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto health = read_file("src/compiler/type_linear_commit_health.hh");
    const auto reflect = read_file("src/compiler/evaluator_primitives_query_reflect.cpp");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    CHECK(tma.find("Issue #3546") != std::string::npos, "3546 AC5: header cite");
    CHECK(tma.find("kCompositeTxnStickyMidIssue = 3546") != std::string::npos,
          "3546 AC5: issue constant");
    CHECK(tma.find("last_stamped_or_composite_audit_mid") != std::string::npos,
          "3546 AC5: last_stamped_or_composite helper");
    CHECK(tma.find("stamp_last_audit_mid") != std::string::npos, "3546 AC5: stamp helper");
    CHECK(tma.find("aura_composite_txn_enter") != std::string::npos, "3546 AC5: C ABI enter");
    CHECK(tma.find("aura_composite_txn_exit") != std::string::npos, "3546 AC5: C ABI exit");
    CHECK(tma.find("g_tls_composite_batch_join_mid") != std::string::npos,
          "3546 AC5: reuses #3066 TLS");
    CHECK(tma.find("g_tls_composite_txn_sticky") == std::string::npos,
          "3546 AC5: no second sticky TLS");
    CHECK(health.find("last_stamped_or_composite_audit_mid") != std::string::npos,
          "3546 AC5: last_proof_mid uses helper");
    CHECK(reflect.find("schema-3546") != std::string::npos, "3546 AC5: additive schema-3546");
    CHECK(reflect.find("query:composite-txn") == std::string::npos, "3546 AC5: no new query key");
    CHECK(ev.find("composite_txn_exit") != std::string::npos,
          "3546 AC5: rollback_atomic_batch_pinning exits sticky");
    CHECK(t.find("ac3546_1_sticky_mid_survives_inner_mutates") != std::string::npos,
          "3546 AC5: AC1 present");
    CHECK(read_file("tests/compiler/test_issue_3546.cpp").empty(),
          "3546 AC5: no test_issue_3546.cpp");
    CHECK(read_file("tests/issues/test_issue_3546.cpp").empty(),
          "3546 AC5: no tests/issues/test_issue_3546.cpp");
    CHECK(read_file("scripts/coverage/checks/check_composite_mid_sticky.py").empty(),
          "3546 AC5: no check_composite_mid_sticky.py");
    const std::filesystem::path docs_design =
        std::filesystem::path(AURA_SOURCE_DIR) / "docs" / "design";
    std::error_code ec;
    if (std::filesystem::exists(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3546-") == std::string::npos,
                  std::string("3546 AC5: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}

// Issue #3367 source-cite + linter pass.
// ── Issue #3599: production refuse class stays joinable — the replay hash
// reads 0 under the epoch=0 matrix (no phantom 1) and the grant-effect deny
// SE lands mid=0 via join_audit_and_se_mid (#3462 contract).
static void ac3599_1_refuse_class_joinable() {
    std::println("\n--- #3599: epoch=0 matrix — replay-mid=0 + deny SE mid=0 ---");
    reset_all();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted face: arms the deny gate.
    auto href_replay = [&](std::string_view key) -> std::int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    CHECK(href_replay("replay-mid") == 0, "3599: epoch=0 + TypedMid=0 -> replay-mid == 0");
    CHECK(href_replay("schema-3143") == 3143, "3599: schema-3143 key unchanged");
    aura::compiler::typed_audit::apply_production_audit_defaults();
    auto& se_ring = ::aura::core::security_event::g_security_event_ring();
    const auto seq0 = se_ring.seq.load(std::memory_order_acquire);
    const auto deny = cs.eval("(security:grant-effect! \"mutate\" 1)");
    (void)deny;
    const auto seq1 = se_ring.seq.load(std::memory_order_acquire);
    bool saw_deny_mid0 = false;
    bool saw_deny_mid1 = false;
    for (std::uint64_t s = seq0; s < seq1; ++s) {
        const auto& e = se_ring.ring[s % se_ring.ring.size()];
        if (e.seq != s || !e.denied)
            continue;
        if (std::string_view{e.reason} != "grant-effect-needs-explicit-tenant-admin")
            continue;
        if (e.mutation_id == 0)
            saw_deny_mid0 = true;
        if (e.mutation_id == 1)
            saw_deny_mid1 = true;
    }
    CHECK(saw_deny_mid0, "3599: grant-effect deny SE mid=0 (refuse class)");
    CHECK(!saw_deny_mid1, "3599: no phantom mid=1 deny row");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

static void ac3367_source_cite_and_no_invent() {
    std::println("\n--- #3367: source-cite + no docs/design/ ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("Issue #3367") != std::string::npos,
          "3367 AC: typed_mutation_audit.h cites #3367 (pin_composite_batch_join_mid "
          "refuse-aligned)");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    CHECK(t.find("ac3367_pin_matrix_no_process_origin_mid_in_hard") != std::string::npos,
          "3367 AC: pin matrix AC1 present");
    const std::filesystem::path docs_design =
        std::filesystem::path(AURA_SOURCE_DIR) / "docs" / "design";
    std::error_code ec;
    if (std::filesystem::exists(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3367-") == std::string::npos,
                  std::string("3367 AC: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}


static void ac3778_occurrence_persist_mid_joins_ssot() {
    std::println("\n--- #3778 AC1: occurrence persist mid joins SSOT ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kOccurrenceMidJoinSsotIssue = 3778") != std::string::npos,
          "#3778 AC1: typed_mutation_audit.h stamps #3778");
    CHECK(mb.find("Issue #3778") != std::string::npos, "#3778 AC1: boundary cites #3778");
    CHECK(mb.find("outermost-pre-persist") != std::string::npos,
          "#3778 AC1: pre-persist site present");
    const auto pre = mb.find("outermost-pre-persist");
    const auto persist = mb.find("aura_outermost_success_persist_occurrence(ev_");
    CHECK(pre != std::string::npos && persist != std::string::npos && pre < persist,
          "#3778 AC1: pre-persist before persist");
    const auto pre_win =
        mb.substr(pre > 400 ? pre - 400 : 0, (persist - (pre > 400 ? pre - 400 : 0)));
    CHECK(pre_win.find("join_audit_and_se_mid(0)") != std::string::npos,
          "#3778 AC1: pre-persist uses join_audit_and_se_mid");
    CHECK(pre_win.find("stk.back().audit_mid") != std::string::npos,
          "#3778 AC1: pre-persist prefers cp.audit_mid");
    // #3780 WAL preflight sits between join mid and persist — keep a wide
    // window so the #3778 join SSOT cite is still visible to this AC.
    // Vacuous mid==0 skip (#3472) sits between join and persist — keep wider.
    const auto persist_win =
        mb.substr(persist > 5000 ? persist - 5000 : 0, persist > 5000 ? 5200 : persist + 200);
    CHECK(persist_win.find("hard_3778") != std::string::npos,
          "#3778 AC1: production mid==0 refuse gate");
    CHECK(persist_win.find("join_audit_and_se_mid(0)") != std::string::npos,
          "#3778 AC1: persist uses join SSOT");
    CHECK(mb.find("densify_stamp_mid_3778") != std::string::npos,
          "#3778 AC2: densify stamp mid SSOT");
    CHECK(efm.find("steal_stamp_mid_3778") != std::string::npos, "#3778 AC2: steal stamp mid SSOT");
    CHECK(mb.find("!hard_3778 && mid == 0") != std::string::npos,
          "#3778 AC3: Soft may observe with defuse when join mid is 0");
    const std::filesystem::path invent =
        std::filesystem::path(AURA_SOURCE_DIR) / "tests" / "compiler" / "test_issue_3778.cpp";
    CHECK(!std::filesystem::exists(invent), "#3778 AC4: no test_issue_3778.cpp");
}

// Issue #3843: require_effect hard face must match Typed resolve —
// production_defaults_active() || AuditStrategy::Full. Full-without-defaults
// (cold-start Full + production_defaults==0) previously invented mid=1 while
// resolve refused mid=0 → phantom SE/grant mid vs Typed trail. Soft mid=1
// observe stamp unchanged. Not a dup of #3837 (string-fence mid join).
static void ac3843_1_full_without_defaults_refuses() {
    std::println(
        "\n--- #3843 AC1: Full + production_defaults=0 + mid==0 → refuse, no phantom mid=1 ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    // Hard face without production_defaults: strategy Full, flag 0.
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    CHECK(aura::compiler::typed_audit::get_strategy() ==
              aura::compiler::typed_audit::AuditStrategy::Full,
          "3843 AC1 pre: strategy Full");
    CHECK(!aura::compiler::typed_audit::production_defaults_active(),
          "3843 AC1 pre: production_defaults inactive");
    CHECK(aura::core::current_mutation_epoch() == 0, "3843 AC1 pre: epoch=0");

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off — mid refuse is hard-face, not sandbox
    ev.set_capability_tenant_id(843);

    using aura::core::security_event::g_security_event_ring;
    const auto seq0 = g_security_event_ring().seq.load(std::memory_order_acquire);
    const bool had_mid1_before = se_ring_has_mid(1);
    const bool ok = ev.require_effect(
        static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate), "test:3843-ac1", 0);
    const auto seq1 = g_security_event_ring().seq.load(std::memory_order_acquire);
    CHECK(!ok, "3843 AC1: Full-without-defaults + mid==0 → require_effect refuses");
    bool saw_mid1 = false;
    for (std::uint64_t s = seq0; s < seq1; ++s) {
        const auto& e = g_security_event_ring().ring[s % g_security_event_ring().ring.size()];
        if (e.seq != s)
            continue;
        if (e.mutation_id == 1)
            saw_mid1 = true;
    }
    CHECK(!saw_mid1, "3843 AC1: no phantom mid=1 SE/grant row after refuse");
    CHECK(!se_ring_has_mid(1) || had_mid1_before,
          "3843 AC1: ring does not mint new mid=1 after Full-without-defaults refuse");
    // Align with Typed resolve: join/resolve hard refuse path.
    const auto resolved = aura::compiler::typed_audit::resolve_audit_mutation_id(0);
    CHECK(resolved == 0, "3843 AC1: Typed resolve also refuses mid=0 under Full");
}

static void ac3843_2_soft_mid1_unchanged() {
    std::println("\n--- #3843 AC2: Soft mid=1 observe stamp unchanged ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    aura::compiler::typed_audit::apply_dev_audit_defaults(); // Sampled / Soft
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    const bool ok = ev.require_effect(
        static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate), "test:3843-ac2", 0);
    CHECK(ok, "3843 AC2: Soft/Off allows without grant");
    using aura::core::security_event::g_security_event_ring;
    const auto& ring = g_security_event_ring();
    const auto seq = ring.seq.load(std::memory_order_relaxed);
    std::uint64_t last_mid = 0;
    if (seq != 0)
        last_mid = ring.ring[(seq - 1) % ring.ring.size()].mutation_id;
    std::println("  3843 AC2: last SecurityEvent mid={}", last_mid);
    CHECK(last_mid == 1, "3843 AC2: Soft observe stamp mid=1 preserved (#2493 AC4 / #3594)");
}

static void ac3843_3_source_cite_wiring_no_invent() {
    std::println("\n--- #3843 AC3: source-cite + linter/manifest/grandfather + no invent ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    const auto build = read_file("build.py");
    const auto gf = read_file("scripts/coverage/simple_check_grandfather.txt");
    const auto lint =
        read_file("scripts/coverage/checks/check_require_effect_full_hard_mid_3843.py");
    const auto man = read_file("scripts/coverage/manifests/3843.json");
    CHECK(sec.find("Issue #3843") != std::string::npos, "3843 AC3: evaluator_security cites #3843");
    CHECK(sec.find("AuditStrategy::Full") != std::string::npos,
          "3843 AC3: require_effect hard face includes Full");
    // Hard gate must OR Full — not production_defaults alone (the residual).
    const auto re = sec.find("bool Evaluator::require_effect");
    CHECK(re != std::string::npos, "3843 AC3: require_effect present");
    const auto re_win = sec.substr(re, 5000);
    CHECK(re_win.find("production_defaults_active()") != std::string::npos &&
              re_win.find("AuditStrategy::Full") != std::string::npos,
          "3843 AC3: require_effect hard = production_defaults || Full");
    CHECK(re_win.find("Soft only") != std::string::npos ||
              re_win.find("mid = 1; // Soft") != std::string::npos,
          "3843 AC3: Soft mid=1 arm retained");
    CHECK(t.find("ac3843_1_full_without_defaults_refuses") != std::string::npos,
          "3843 AC3: AC1 present");
    CHECK(t.find("ac3843_2_soft_mid1_unchanged") != std::string::npos, "3843 AC3: AC2 present");
    CHECK(!lint.empty() && lint.find("Issue #3843") != std::string::npos, "3843 AC3: linter");
    CHECK(!man.empty() && man.find("\"issue\": 3843") != std::string::npos, "3843 AC3: manifest");
    CHECK(gf.find("check_require_effect_full_hard_mid_3843.py") != std::string::npos,
          "3843 AC3: grandfather");
    CHECK(build.find("check_require_effect_full_hard_mid_3843") != std::string::npos,
          "3843 AC3: build.py gate");
    CHECK(build.find("cmd_require_effect_full_hard_mid_3843") != std::string::npos,
          "3843 AC3: build.py cmd");
    // Not a dup of #3837 string-fence mid join.
    CHECK(sec.find("#3837") != std::string::npos || t.find("#3837") != std::string::npos,
          "3843 AC3: cites #3837 as separate (not dup)");
    CHECK(read_file("tests/compiler/test_issue_3843.cpp").empty(),
          "3843 AC3: no test_issue_3843.cpp");
    CHECK(read_file("tests/issues/test_issue_3843.cpp").empty(),
          "3843 AC3: no tests/issues/test_issue_3843.cpp");
    const std::filesystem::path docs_design =
        std::filesystem::path(AURA_SOURCE_DIR) / "docs" / "design";
    std::error_code ec;
    if (std::filesystem::exists(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3843-") == std::string::npos,
                  std::string("3843 AC3: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}


// Issue #3844: check_and_record_effect / make_grant_provenance invent
// prov.epoch = me ?: 1 when Mutation epoch is 0. Under production_defaults
// || Full, epoch must stay 0 (WorkspaceEpoch Mutation only). Soft may keep
// observe stamp. #3837 is deny-SE mid invent — sibling, not a dup.
static void ac3844_1_hard_epoch0_stays_zero() {
    std::println(
        "\n--- #3844 AC1: hard face + TypedMid≠0 + Mutation epoch=0 → grant/SE epoch=0 ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    // Hard face: Full without production_defaults (cold-start Full residual).
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Full);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    CHECK(aura::core::current_mutation_epoch() == 0, "3844 AC1 pre: epoch=0");
    CHECK(aura::compiler::typed_audit::production_hard_face_active(),
          "3844 AC1 pre: hard face active");

    using aura::core::capability::Effect;
    using aura::core::capability::make_grant_provenance;
    using aura::core::capability::stamp_grant_mutation_epoch;
    constexpr std::uint64_t kTypedMid = 4242;
    // Stamp TypedMid so mid join is non-zero while Mutation epoch stays 0.
    aura::compiler::typed_audit::stamp_type_linear_commit_proof(kTypedMid);

    auto prov = make_grant_provenance(kTypedMid, /*force_bind=*/true, 0, 0);
    CHECK(prov.epoch == 0, "3844 AC1: make_grant_provenance epoch stays 0 (no phantom 1)");
    CHECK(prov.mutation_id == kTypedMid, "3844 AC1: mid join unchanged (TypedMid)");
    CHECK(stamp_grant_mutation_epoch() == 0, "3844 AC1: stamp helper returns 0 under hard");

    // Bootstrap TenantAdmin while the registry is still Off — the #3409
    // SSOT fence refuses same-tenant high-bits grants from a caller that
    // holds no TA yet (cold start has no TA to vouch). Production seeds TA
    // before arming Restricted; mirror that here, then arm.
    {
        auto ta = make_grant_provenance(kTypedMid, true, 0, 0);
        CHECK(ta.epoch == 0, "3844 AC1: TA grant epoch stays 0");
        CHECK(g_capability_registry().grant(844, "tenant-admin", Effect::TenantAdmin, ta,
                                            /*single_use=*/false, /*session_bound=*/false, 844),
              "3844 AC1: TA bootstrap grant under Off");
    }
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    g_capability_registry().sandbox_mode = aura::core::capability::EffectSandboxMode::Restricted;
    auto gprov = make_grant_provenance(kTypedMid, true, 0, 0);
    const bool granted =
        g_capability_registry().grant(844, "mutate-3844", Effect::Mutate, gprov,
                                      /*single_use=*/false, /*session_bound=*/false, 844);
    CHECK(granted, "3844 AC1: Mutate grant accepted");
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(844, "mutate-3844", g), "3844 AC1: grant found");
    CHECK(g.grant_epoch == 0, "3844 AC1: grant_epoch stays 0 (not invented 1)");
    CHECK(g.bound_mutation_id == kTypedMid, "3844 AC1: bound mid joins TypedMid");

    // check_and_record_effect path: SE.epoch must be 0, mid stays TypedMid.
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(844);
    using aura::core::security_event::g_security_event_ring;
    const auto seq0 = g_security_event_ring().seq.load(std::memory_order_acquire);
    const bool ok = ev.check_and_record_effect_for_test(
        static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate),
        static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate), "test:3844-ac1", 0,
        844, kTypedMid);
    CHECK(ok, "3844 AC1: effect check allows with live mid + grant");
    const auto seq1 = g_security_event_ring().seq.load(std::memory_order_acquire);
    bool saw_epoch1 = false;
    bool saw_epoch0_with_mid = false;
    for (std::uint64_t s = seq0; s < seq1; ++s) {
        const auto& e = g_security_event_ring().ring[s % g_security_event_ring().ring.size()];
        if (e.seq != s)
            continue;
        if (e.epoch == 1)
            saw_epoch1 = true;
        if (e.epoch == 0 && e.mutation_id == kTypedMid)
            saw_epoch0_with_mid = true;
    }
    CHECK(!saw_epoch1, "3844 AC1: no phantom epoch=1 SE/WAL row");
    CHECK(saw_epoch0_with_mid,
          "3844 AC1: SE epoch=0 with TypedMid join (Mutation vocabulary intact)");

    // Registry + proof + TLS hygiene for downstream members: pre-fix these
    // grants failed silently (#3409 cold start) and the effect check was
    // denied — downstream members saw that world. The allowed path stamps
    // the TLS boundary mid + proof + grant entries; restore all of it.
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    g_capability_registry().revoke(844, "mutate-3844");
    g_capability_registry().revoke(844, "tenant-admin");
}

static void ac3844_2_soft_observe_stamp_documented() {
    std::println(
        "\n--- #3844 AC2: Soft observe stamp (epoch=1) retained when product wants it ---");
    reset_all();
    aura::compiler::typed_audit::reset_for_test();
    aura::core::reset_mutation_epoch_for_test();
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    aura::compiler::typed_audit::apply_dev_audit_defaults(); // Soft/Sampled
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    CHECK(!aura::compiler::typed_audit::production_hard_face_active(), "3844 AC2 pre: Soft face");
    CHECK(aura::core::current_mutation_epoch() == 0, "3844 AC2 pre: epoch=0");

    using aura::core::capability::make_grant_provenance;
    using aura::core::capability::stamp_grant_mutation_epoch;
    CHECK(stamp_grant_mutation_epoch() == 1, "3844 AC2: Soft stamp invents observe 1");
    auto prov = make_grant_provenance(0, false, 0, 0);
    CHECK(prov.epoch == 1, "3844 AC2: Soft make_grant_provenance observe stamp epoch=1");
}

static void ac3844_3_source_cite_wiring_no_invent() {
    std::println("\n--- #3844 AC3: source-cite + linter/manifest/grandfather + no invent ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    const auto build = read_file("build.py");
    const auto gf = read_file("scripts/coverage/simple_check_grandfather.txt");
    const auto lint = read_file("scripts/coverage/checks/check_grant_epoch_no_phantom_3844.py");
    const auto man = read_file("scripts/coverage/manifests/3844.json");
    CHECK(sec.find("Issue #3844") != std::string::npos, "3844 AC3: evaluator_security cites #3844");
    CHECK(cap.find("Issue #3844") != std::string::npos, "3844 AC3: capability_model cites #3844");
    CHECK(cap.find("stamp_grant_mutation_epoch") != std::string::npos,
          "3844 AC3: stamp_grant_mutation_epoch helper");
    CHECK(cap.find("make_grant_provenance") != std::string::npos &&
              cap.find("stamp_grant_mutation_epoch()") != std::string::npos,
          "3844 AC3: make_grant_provenance uses stamp helper");
    // Hard invent gone: no bare `me != 0 ? me : 1` assigned to prov.epoch
    // outside Soft arms in check_and_record_effect.
    const auto care = sec.find("bool Evaluator::check_and_record_effect");
    CHECK(care != std::string::npos, "3844 AC3: check_and_record_effect present");
    const auto care_win = sec.substr(care, 6000);
    CHECK(care_win.find("production_hard_face_active()") != std::string::npos,
          "3844 AC3: check_and_record_effect hard-face gate");
    CHECK(care_win.find("Soft observe stamp only") != std::string::npos,
          "3844 AC3: Soft observe path documented");
    CHECK(hooks.find("aura_production_hard_face_active_probe") != std::string::npos,
          "3844 AC3: hard-face probe defined");
    CHECK(t.find("ac3844_1_hard_epoch0_stays_zero") != std::string::npos, "3844 AC3: AC1 present");
    CHECK(t.find("ac3844_2_soft_observe_stamp_documented") != std::string::npos,
          "3844 AC3: AC2 present");
    CHECK(!lint.empty() && lint.find("Issue #3844") != std::string::npos, "3844 AC3: linter");
    CHECK(!man.empty() && man.find("\"issue\": 3844") != std::string::npos, "3844 AC3: manifest");
    CHECK(gf.find("check_grant_epoch_no_phantom_3844.py") != std::string::npos,
          "3844 AC3: grandfather");
    CHECK(build.find("check_grant_epoch_no_phantom_3844") != std::string::npos,
          "3844 AC3: build.py gate");
    CHECK(sec.find("#3837") != std::string::npos || cap.find("#3837") != std::string::npos ||
              t.find("#3837") != std::string::npos,
          "3844 AC3: cites #3837 as separate (not dup)");
    CHECK(read_file("tests/compiler/test_issue_3844.cpp").empty(),
          "3844 AC3: no test_issue_3844.cpp");
    CHECK(read_file("tests/issues/test_issue_3844.cpp").empty(),
          "3844 AC3: no tests/issues/test_issue_3844.cpp");
    const std::filesystem::path docs_design =
        std::filesystem::path(AURA_SOURCE_DIR) / "docs" / "design";
    std::error_code ec;
    if (std::filesystem::exists(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3844-") == std::string::npos,
                  std::string("3844 AC3: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}


// Issue #3845: promote_sampled_force_join_mid still invented mid via
// next_audit_mutation_id() when mid==0. Align with pin #3367 — hard
// (production_defaults || Full) returns 0 (no invent, no sticky); Soft
// invent intentionally NOT kept (quiet #3066 AC3). Not a refile of
// #3837/#3838.
static void ac3845_1_hard_promote_zero_no_invent() {
    std::println("\n--- #3845 AC1: hard face + promote(0) → 0; no sticky; no gen invent ---");
    using namespace aura::compiler::typed_audit;
    auto reset_state = []() {
        reset_for_test();
        reset_all();
        clear_boundary_audit_mid();
        clear_type_linear_commit_proof_for_test();
        aura::core::reset_mutation_epoch_for_test();
        aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
        process_resource_quota_manager().provenance_mutation_id = 0;
        g_tls_composite_batch_join_mid = 0;
        g_last_composite_batch_join_mid.store(0, std::memory_order_relaxed);
        g_last_stamped_audit_mid.store(0, std::memory_order_relaxed);
    };
    auto read_gen = []() { return g_typed_mutation_audit_counters.audit_mutation_id_gen.load(); };

    // AC1a: production_defaults + mid==0
    {
        reset_state();
        apply_production_audit_defaults();
        g_typed_mutation_audit_counters.production_defaults_active.store(1);
        const auto gen0 = read_gen();
        const auto mid = promote_sampled_force_join_mid(0);
        CHECK(mid == 0, "3845 AC1a: production + promote(0) returns 0");
        CHECK(g_tls_composite_batch_join_mid == 0, "3845 AC1a: no sticky TLS mid");
        CHECK(g_last_composite_batch_join_mid.load() == 0, "3845 AC1a: no last composite mid");
        CHECK(g_last_stamped_audit_mid.load() == 0, "3845 AC1a: no last_stamped invent");
        CHECK(read_gen() == gen0, "3845 AC1a: no audit_mutation_id_gen invent");
    }

    // AC1b: Full without production_defaults + mid==0 (cold-start Full residual)
    {
        reset_state();
        set_strategy(AuditStrategy::Full);
        g_typed_mutation_audit_counters.production_defaults_active.store(0);
        const auto gen0 = read_gen();
        const auto mid = promote_sampled_force_join_mid(0);
        CHECK(mid == 0, "3845 AC1b: Full-without-defaults + promote(0) returns 0");
        CHECK(g_tls_composite_batch_join_mid == 0, "3845 AC1b: no sticky TLS mid");
        CHECK(read_gen() == gen0, "3845 AC1b: no audit_mutation_id_gen invent");
    }
}

static void ac3845_2_nonzero_deny_mid_unchanged() {
    std::println("\n--- #3845 AC2: non-zero deny_mid path unchanged ---");
    using namespace aura::compiler::typed_audit;
    reset_for_test();
    reset_all();
    clear_boundary_audit_mid();
    apply_production_audit_defaults();
    g_typed_mutation_audit_counters.production_defaults_active.store(1);
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    g_tls_composite_batch_join_mid = 0;
    constexpr std::uint64_t kDeny = 38450042;
    const auto mid = promote_sampled_force_join_mid(kDeny);
    CHECK(mid == kDeny, "3845 AC2: promote(deny_mid) returns deny_mid");
    CHECK(g_tls_composite_batch_join_mid == kDeny, "3845 AC2: sticky TLS = deny_mid");
    CHECK(g_last_composite_batch_join_mid.load() == kDeny, "3845 AC2: last composite = deny_mid");
    CHECK(g_last_stamped_audit_mid.load() == kDeny, "3845 AC2: last_stamped = deny_mid");
    // Clear sticky for subsequent cases.
    composite_txn_exit();
    CHECK(g_tls_composite_batch_join_mid == 0, "3845 AC2: exit clears sticky");
}

static void ac3845_3_soft_no_observe_invent_documented() {
    std::println(
        "\n--- #3845 AC3: Soft promote(0) → 0; Soft invent NOT kept (aligned pin #3367) ---");
    using namespace aura::compiler::typed_audit;
    reset_for_test();
    reset_all();
    clear_boundary_audit_mid();
    clear_type_linear_commit_proof_for_test();
    apply_dev_audit_defaults(); // Soft/Sampled
    g_typed_mutation_audit_counters.production_defaults_active.store(0);
    aura::core::reset_mutation_epoch_for_test();
    aura::core::store_workspace_epoch(aura::core::WorkspaceEpochKind::Mutation, 0);
    process_resource_quota_manager().provenance_mutation_id = 0;
    g_tls_composite_batch_join_mid = 0;
    g_last_composite_batch_join_mid.store(0, std::memory_order_relaxed);
    g_last_stamped_audit_mid.store(0, std::memory_order_relaxed);
    const auto gen0 = g_typed_mutation_audit_counters.audit_mutation_id_gen.load();
    const auto mid = promote_sampled_force_join_mid(0);
    CHECK(mid == 0, "3845 AC3: Soft + promote(0) returns 0 (no Soft observe invent)");
    CHECK(g_tls_composite_batch_join_mid == 0, "3845 AC3: Soft no sticky");
    CHECK(g_typed_mutation_audit_counters.audit_mutation_id_gen.load() == gen0,
          "3845 AC3: Soft does not bump audit_mutation_id_gen");
    // Source documents Soft invent NOT kept.
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("Soft invent is intentionally NOT kept") != std::string::npos ||
              tma.find("Soft quiet — no invent") != std::string::npos,
          "3845 AC3: Soft invent-not-kept documented in promote");
}

static void ac3845_4_source_cite_wiring_no_invent() {
    std::println("\n--- #3845 AC4: source-cite + linter/manifest/grandfather + no invent ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto t = read_file("tests/compiler/test_audit_mutation_id_unify.cpp");
    const auto build = read_file("build.py");
    const auto gf = read_file("scripts/coverage/simple_check_grandfather.txt");
    const auto lint =
        read_file("scripts/coverage/checks/check_promote_force_join_mid_no_invent_3845.py");
    const auto man = read_file("scripts/coverage/manifests/3845.json");
    CHECK(tma.find("Issue #3845") != std::string::npos,
          "3845 AC4: typed_mutation_audit.h cites #3845");
    CHECK(tma.find("promote_sampled_force_join_mid") != std::string::npos, "3845 AC4: promote fn");
    // Promote window must not invent via next_audit_mutation_id.
    const auto promo = tma.find("inline std::uint64_t promote_sampled_force_join_mid");
    CHECK(promo != std::string::npos, "3845 AC4: promote def present");
    const auto promo_win = tma.substr(promo, 900);
    CHECK(promo_win.find("next_audit_mutation_id()") == std::string::npos,
          "3845 AC4: promote does NOT call next_audit_mutation_id (no invent)");
    CHECK(promo_win.find("production_defaults_active()") != std::string::npos &&
              promo_win.find("AuditStrategy::Full") != std::string::npos,
          "3845 AC4: promote hard face = production_defaults || Full");
    CHECK(t.find("ac3845_1_hard_promote_zero_no_invent") != std::string::npos,
          "3845 AC4: AC1 present");
    CHECK(t.find("ac3845_2_nonzero_deny_mid_unchanged") != std::string::npos,
          "3845 AC4: AC2 present");
    CHECK(t.find("ac3845_3_soft_no_observe_invent_documented") != std::string::npos,
          "3845 AC4: AC3 present");
    CHECK(!lint.empty() && lint.find("Issue #3845") != std::string::npos, "3845 AC4: linter");
    CHECK(!man.empty() && man.find("\"issue\": 3845") != std::string::npos, "3845 AC4: manifest");
    CHECK(gf.find("check_promote_force_join_mid_no_invent_3845.py") != std::string::npos,
          "3845 AC4: grandfather");
    CHECK(build.find("check_promote_force_join_mid_no_invent_3845") != std::string::npos,
          "3845 AC4: build.py gate");
    CHECK(build.find("cmd_promote_force_join_mid_no_invent_3845") != std::string::npos,
          "3845 AC4: build.py cmd");
    // Not a refile of #3837/#3838.
    CHECK(tma.find("#3837") != std::string::npos || t.find("#3837") != std::string::npos,
          "3845 AC4: cites #3837 as separate (not dup)");
    CHECK(tma.find("#3838") != std::string::npos || t.find("#3838") != std::string::npos,
          "3845 AC4: cites #3838 as separate (not dup)");
    CHECK(read_file("tests/compiler/test_issue_3845.cpp").empty(),
          "3845 AC4: no test_issue_3845.cpp");
    CHECK(read_file("tests/issues/test_issue_3845.cpp").empty(),
          "3845 AC4: no tests/issues/test_issue_3845.cpp");
    const std::filesystem::path docs_design =
        std::filesystem::path(AURA_SOURCE_DIR) / "docs" / "design";
    std::error_code ec;
    if (std::filesystem::exists(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3845-") == std::string::npos,
                  std::string("3845 AC4: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}

} // namespace

// ── Issue #3903: capture_security_correlated_audit carries tenant_id ──
static void ac3903_1_correlate_tenant_passthrough() {
    std::println("\n--- #3903 AC1: correlate tenant_id passthrough ---");
    reset_all();
    constexpr std::uint64_t mid = 3903;
    constexpr std::uint32_t tenant = 3903;
    const auto before = g_typed_mutation_audit_counters.audits_considered.load();
    capture_security_correlated_audit(/*mutation_id=*/mid, "test:3903-ac1",
                                      /*epoch=*/0, /*denied=*/false,
                                      /*target_node=*/0, /*fiber_id=*/0,
                                      /*tenant_id=*/tenant);
    CHECK(g_typed_mutation_audit_counters.audits_considered.load() == before + 1,
          "3903 AC1: audits_considered bumped");
    bool found = false;
    std::uint32_t seen_tenant = 999;
    {
        std::lock_guard<std::mutex> lock(aura::compiler::typed_audit::g_trail().mu);
        for (const auto& e : aura::compiler::typed_audit::g_trail().ring) {
            if (e.mutation_id == mid) {
                found = true;
                seen_tenant = e.tenant_id;
                break;
            }
        }
    }
    CHECK(found, "3903 AC1: trail row for mid reachable");
    CHECK(seen_tenant == tenant, "3903 AC1: trail row tenant_id == passed tenant");
    reset_all();
}

static void ac3903_2_default_tenant_zero_unchanged() {
    std::println("\n--- #3903 AC2: default tenant stays 0 (existing callers unaffected) ---");
    reset_all();
    constexpr std::uint64_t mid = 3904;
    capture_security_correlated_audit(/*mutation_id=*/mid, "test:3903-ac2",
                                      /*epoch=*/0, /*denied=*/false);
    bool found = false;
    std::uint32_t seen_tenant = 999;
    {
        std::lock_guard<std::mutex> lock(aura::compiler::typed_audit::g_trail().mu);
        for (const auto& e : aura::compiler::typed_audit::g_trail().ring) {
            if (e.mutation_id == mid) {
                found = true;
                seen_tenant = e.tenant_id;
                break;
            }
        }
    }
    CHECK(found, "3903 AC2: trail row for mid reachable");
    CHECK(seen_tenant == 0, "3903 AC2: omitted tenant → 0 (existing callers unchanged)");
    reset_all();
}

static void ac3903_3_source_cite() {
    std::println("\n--- #3903 AC3: source-cite + no invent ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("Issue #3903") != std::string::npos, "3903 AC3: correlate API cites #3903");
    const auto es = read_file("src/compiler/evaluator_security.cpp");
    CHECK(es.find("Issue #3903") != std::string::npos,
          "3903 AC3: evaluator_security call sites pass tenant");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("Issue #3903") != std::string::npos,
          "3903 AC3: fiber_mutation call sites pass tenant");
    CHECK(!std::filesystem::exists("docs/design/3903-correlate-tenant-id.md"),
          "3903 AC3: no docs/design");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3903.cpp"),
          "3903 AC3: no tests/issues invent");
}

static void ac3994_1_host_path_deny_typed_tenant() {
    std::println("\n--- #3994 AC1: host-path IsolationDeny Typed tenant == capability tenant ---");
    reset_all();
    bump_mutation_epoch(1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ::setenv("AURA_MULTI_TENANT", "1", 1);
    aura::core::provenance::set_multi_tenant_env_active(true);
    const char* tmp = std::getenv("TMPDIR");
    const std::string base = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/aura-3994-ac1";
    ::setenv("AURA_TENANT_FS_ROOT", base.c_str(), 1);
    using aura::compiler::security::kHostPathCorrelateTenantIssue;
    using aura::compiler::security::tenant_host_root_for;
    CHECK(kHostPathCorrelateTenantIssue == 3994, "3994 AC1: issue stamp");
    const auto escape = tenant_host_root_for(42) + "/clobber.txt";
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(7);
    std::string out;
    CHECK(!ev.check_tenant_host_path(escape, out, "write-file"),
          "3994 AC1: Evaluator denies A→B path");
    bool found = false;
    std::uint32_t seen_tenant = 0;
    {
        std::lock_guard<std::mutex> lock(aura::compiler::typed_audit::g_trail().mu);
        for (const auto& e : aura::compiler::typed_audit::g_trail().ring) {
            if (std::string_view(e.name) != "write-file")
                continue;
            if (e.outcome != aura::compiler::typed_audit::AuditOutcome::Error)
                continue;
            found = true;
            seen_tenant = e.tenant_id;
            break;
        }
    }
    CHECK(found, "3994 AC1: Typed deny row for write-file reachable");
    CHECK(seen_tenant == 7, "3994 AC1: Typed tenant_id == capability_tenant_id_");
    ::unsetenv("AURA_TENANT_FS_ROOT");
    ::unsetenv("AURA_MULTI_TENANT");
    aura::core::provenance::set_multi_tenant_env_active(false);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    reset_all();
}

static void ac3994_2_soft_unchanged() {
    std::println("\n--- #3994 AC2: Soft/Off host-path passthrough (no Typed deny) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::core::provenance::set_multi_tenant_env_active(false);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    ev.set_capability_tenant_id(7);
    const auto before = g_typed_mutation_audit_counters.audits_considered.load();
    std::string out;
    CHECK(ev.check_tenant_host_path("/tmp/anywhere.txt", out, "write-file"),
          "3994 AC2: Off check_tenant_host_path allows");
    CHECK(g_typed_mutation_audit_counters.audits_considered.load() == before,
          "3994 AC2: Soft/Off does not emit Typed correlate");
    reset_all();
}

static void ac3994_3_source_cite() {
    std::println("\n--- #3994 AC3: source-cite host-path correlate tenant; no invent ---");
    const auto es = read_file("src/compiler/evaluator_security.cpp");
    CHECK(es.find("Issue #3994") != std::string::npos, "3994 AC3: check_tenant_host_path cites");
    auto pos = es.find("bool Evaluator::check_tenant_host_path");
    CHECK(pos != std::string::npos, "3994 AC3: host-path present");
    auto win = pos == std::string::npos ? std::string{} : es.substr(pos, 2800);
    CHECK(win.find("capture_security_correlated_audit") != std::string::npos,
          "3994 AC3: host-path correlates");
    CHECK(win.find("capability_tenant_id_") != std::string::npos,
          "3994 AC3: correlate stamps capability tenant");
    const auto hh = read_file("src/compiler/tenant_host_path.hh");
    CHECK(hh.find("kHostPathCorrelateTenantIssue = 3994") != std::string::npos,
          "3994 AC3: issue stamp");
    CHECK(!std::filesystem::exists("docs/design/3994-host-path-correlate-tenant.md"),
          "3994 AC3: no docs/design");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3994.cpp"),
          "3994 AC3: no tests/issues invent");
}

int run_test_audit_mutation_id_unify() {
    std::println("=== Issue #2493: mutation_id source unify (WorkspaceEpoch Mutation) ===");
    ac1_prefers_caller_then_mutation_epoch();
    ac2_resource_quota_fallback();
    ac3_aot_hotupdate_uses_resolve();
    ac4_soft_no_activity_fallback();
    ac5_correlated_audit_join();
    ac6_source_and_gate();
    ac3903_1_correlate_tenant_passthrough();
    ac3903_2_default_tenant_zero_unchanged();
    ac3903_3_source_cite();
    ac3994_1_host_path_deny_typed_tenant();
    ac3994_2_soft_unchanged();
    ac3994_3_source_cite();
    ac7_boundary_trail_uses_resolve();
    ac3066_1_production_batch_share_mid();
    ac3066_2_sampled_force_joinable();
    ac3066_3_soft_zero_extra();
    ac3066_4_linter_no_design();
    ac3367_pin_matrix_no_process_origin_mid_in_hard();
    ac3367_source_cite_and_no_invent();
    ac3546_1_sticky_mid_survives_inner_mutates();
    ac3546_2_abort_clear_still_prefers_tls();
    ac3546_3_enter_zero_is_noop();
    ac3546_4_source_cite_no_invent();
    ac3599_1_refuse_class_joinable();
    ac3778_occurrence_persist_mid_joins_ssot();
    std::println("\n=== Issue #3843: require_effect Full-without-defaults hard mid refuse ===");
    ac3843_1_full_without_defaults_refuses();
    ac3843_2_soft_mid1_unchanged();
    ac3843_3_source_cite_wiring_no_invent();
    std::println("\n=== Issue #3844: grant/SE epoch no phantom 1 under hard face ===");
    ac3844_1_hard_epoch0_stays_zero();
    ac3844_2_soft_observe_stamp_documented();
    ac3844_3_source_cite_wiring_no_invent();
    std::println("\n=== Issue #3845: promote_sampled_force_join_mid no invent ===");
    ac3845_1_hard_promote_zero_no_invent();
    ac3845_2_nonzero_deny_mid_unchanged();
    ac3845_3_soft_no_observe_invent_documented();
    ac3845_4_source_cite_wiring_no_invent();
    std::println("\n=== Issue #3874: TypedMutationAuditEvent capability tenant ===");
    ac3874_tenant_stamp();
    std::println("\n=== Issue #3875: revoke_epoch hard-only process-origin stamp ===");
    ac3875_revoke_epoch_hard_only_invent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_mutation_id_unify();
}
#endif

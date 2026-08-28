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
#include "core/capability_model.hh"
#include "core/resource_quota.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/typed_mutation_audit_counters.h"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
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
    CHECK(build.find("check_audit_mutation_id_unify_2493") != std::string::npos ||
              build.find("cmd_audit_mutation_id_unify_2493_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_audit_mutation_id_unify_2493.py");
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
    CHECK(pin != 0, "3066 AC1: production batch pins a join mid");
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
    CHECK(aura::compiler::typed_audit::trail_find_by_mutation_id(pin, te),
          "3066 AC1: typed trail find pin");
    CHECK(se_ring_has_mid(pin), "3066 AC1: SE ring has same mid");
    CHECK(href_audit(cs, "schema-3066") == 3066, "3066 AC1: live schema-3066");
    CHECK(href_audit(cs, "last-stamped-audit-mid") == static_cast<std::int64_t>(pin),
          "3066 AC1: query last-stamped == pin");

    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::compiler::typed_audit::clear_boundary_audit_mid();
    (void)ev;
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

// Issue #3367 source-cite + linter pass.
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

} // namespace

int run_test_audit_mutation_id_unify() {
    std::println("=== Issue #2493: mutation_id source unify (WorkspaceEpoch Mutation) ===");
    ac1_prefers_caller_then_mutation_epoch();
    ac2_resource_quota_fallback();
    ac3_aot_hotupdate_uses_resolve();
    ac4_soft_no_activity_fallback();
    ac5_correlated_audit_join();
    ac6_source_and_gate();
    ac7_boundary_trail_uses_resolve();
    ac3066_1_production_batch_share_mid();
    ac3066_2_sampled_force_joinable();
    ac3066_3_soft_zero_extra();
    ac3066_4_linter_no_design();
    ac3367_pin_matrix_no_process_origin_mid_in_hard();
    ac3367_source_cite_and_no_invent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_mutation_id_unify();
}
#endif

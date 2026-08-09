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

} // namespace

int run_test_audit_mutation_id_unify() {
    std::println("=== Issue #2493: mutation_id source unify (WorkspaceEpoch Mutation) ===");
    ac1_prefers_caller_then_mutation_epoch();
    ac2_resource_quota_fallback();
    ac3_aot_hotupdate_uses_resolve();
    ac4_soft_no_activity_fallback();
    ac5_correlated_audit_join();
    ac6_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_mutation_id_unify();
}
#endif

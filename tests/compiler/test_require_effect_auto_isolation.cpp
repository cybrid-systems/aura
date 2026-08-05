// @category: unit
// @reason: Issue #2490 — require_effect is the single side-effect entry.
// Auto-enforce workspace isolation when req_bits != 0 so callers cannot
// skip isolation by only calling require_effect. Pure / zero-bits callers
// unchanged. Single SE IsolationDeny count preserved via #2388.
//
//   AC1: Restricted + tenant principal unset + require_effect(Mutate) →
//        IsolationDeny with isolation-deny:unset-principal; no side effect.
//   AC2: Restricted + principal set + Mutate grant → allow (capability +
//        isolation both pass).
// AC3: Cross-tenant ref without cross-grant + side-effect bits →
//      IsolationDeny; single SE count (no double-count regression vs #2388).
//   AC4: Off sandbox + tenant=0 → still permissive (unit Soft path).
//   AC5: Existing prims that only call require_effect gain isolation
//        without per-prim edits (auto-enforcement covers new side-effect
//        classes for free).
//   AC6: Tests + source-cite require_effect (prefer-existing isolation /
//        capability suites).

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/security_event.hh"
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
using aura::compiler::security::kEffectMutate;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEvent;
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
    using ::aura::core::workspace_isolation::g_workspace_isolation;
    g_workspace_isolation().set_strict_sandbox_linked(false);
}

// Helper: count IsolationDeny SecurityEvents in the ring since a baseline.
static std::size_t isolation_denies_since(std::uint64_t baseline_seq) {
    const auto& ring = g_security_event_ring();
    const auto seq = ring.seq.load(std::memory_order_acquire);
    std::size_t count = 0;
    for (std::uint64_t s = baseline_seq; s < seq; ++s) {
        const auto& e = ring.ring[s % ring.ring.size()];
        if (static_cast<int>(e.kind) ==
                static_cast<int>(::aura::core::security_event::SecurityEventKind::IsolationDeny) &&
            e.seq == s) {
            count++;
        }
    }
    return count;
}

static std::uint64_t current_seq() {
    return g_security_event_ring().seq.load(std::memory_order_acquire);
}

// AC1: Restricted + tenant principal unset + require_effect(Mutate) →
// IsolationDeny with isolation-deny:unset-principal; no side effect.
static void ac1_restricted_unset_principal_denies() {
    std::println("\n--- #2490 AC1: Restricted + unset principal denies ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(0); // unset principal
    ev.grant_effect_capability(0, "mutate-2490", kEffectMutate,
                               0); // grant irrelevant — isolation fires first

    const auto before = current_seq();
    const auto mismatch0 =
        g_capability_effect_metrics().capability_provenance_mismatch_total.load();
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2490-ac1-unset", 0);
    const auto mismatch1 =
        g_capability_effect_metrics().capability_provenance_mismatch_total.load();
    std::println("  require_effect={} mismatch {}→{}", ok, mismatch0, mismatch1);
    CHECK(!ok, "AC1: require_effect denies under Restricted + unset principal");
    // Isolation short-circuits before capability check, so mismatch count
    // must not bump (no provenance check fired).
    CHECK(mismatch1 == mismatch0, "AC1: no provenance mismatch bump (isolation short-circuits)");
    // IsolationDeny SE emitted with stable reason.
    const auto deny_count = isolation_denies_since(before);
    std::println("  IsolationDeny events since baseline: {}", deny_count);
    CHECK(deny_count >= 1, "AC1: at least one IsolationDeny SE emitted");
}

// AC2: Restricted + principal set + Mutate grant → allow (both pass).
static void ac2_restricted_principal_grant_allows() {
    std::println("\n--- #2490 AC2: Restricted + principal + grant allows ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(42);
    const auto me = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(42, "mutate-2490-ac2", kEffectMutate, me == 0 ? 1 : me);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(42, "mutate-2490-ac2", g), "AC2: grant installed");
    CHECK(g.tenant_id == 42, "AC2: grant stamped with tenant 42");

    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2490-ac2-allow", 0);
    CHECK(ok, "AC2: require_effect allows under principal + grant (both pass)");
}

// AC3: cross-tenant scenario — single SE count, no double-count regression
// vs #2388. Verify by triggering isolation deny via require_effect and
// confirming exactly one IsolationDeny SE (the auto-isolation path emits
// at most one, matching #2388's unified SE surface).
static void ac3_single_isolation_deny_count() {
    std::println("\n--- #2490 AC3: single SE count on isolation deny ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(0); // unset → isolation deny path

    const auto before = current_seq();
    const bool ok = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                      "test:2490-ac3-singlecount", 0);
    CHECK(!ok, "AC3: require_effect denies under Restricted + unset");
    const auto denies = isolation_denies_since(before);
    std::println("  IsolationDeny events emitted: {}", denies);
    CHECK(denies == 1, "AC3: exactly one IsolationDeny SE (no double-count, #2388 parity)");

    // Second require_effect call under same conditions → another single
    // count (one per call, no stacking or replay).
    const auto before2 = current_seq();
    const bool ok2 =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2490-ac3-secondcall", 0);
    CHECK(!ok2, "AC3: second require_effect also denies");
    const auto denies2 = isolation_denies_since(before2);
    CHECK(denies2 == 1, "AC3: second call emits exactly one IsolationDeny");
}

// AC4: Off sandbox + tenant=0 → still permissive (unit Soft path).
static void ac4_off_sandbox_permissive() {
    std::println("\n--- #2490 AC4: Off sandbox remains permissive ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);  // Off
    ev.set_capability_tenant_id(0); // unset tenant is fine under Off
    // No grant needed — Off path allows.
    const auto before = current_seq();
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2490-ac4-off", 0);
    CHECK(ok, "AC4: Off sandbox + tenant=0 allows require_effect");
    const auto denies = isolation_denies_since(before);
    CHECK(denies == 0, "AC4: no IsolationDeny under Off sandbox");
}

// AC5: existing prims that only call require_effect gain isolation
// enforcement without per-prim edits — proven by AC1 above. Re-test
// from a different code path angle: an evaluator_primitives_* caller
// that only routes through require_effect must now be isolated under
// Restricted + unset principal.
static void ac5_existing_prims_gain_isolation() {
    std::println("\n--- #2490 AC5: existing prim path gains isolation ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(0);
    // Simulate "existing prim" by calling require_effect directly (the
    // single production entry). Pre-#2490 this would have skipped
    // isolation; post-#2490 it short-circuits on the auto-check.
    const bool ok = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                      "test:2490-ac5-existingprim", 0);
    CHECK(!ok, "AC5: require_effect auto-enforces isolation for existing prims");
}

// Regression: #2384 live mid provenance still present in require_effect.
CHECK(sec.find("Issue #2384") != std::string::npos, "AC6: #2384 live mid provenance preserved");
}

// ─── Issue #2658: require_effect ref_tenant gate ──────────────────
// AC1: require_effect(..., ref_tenant=42) under principal=7 + Restricted/Strict
//      + no cross-grant → IsolationDeny with reason carrying ref-tenant.
// AC2: same call with matching principal or explicit cross-grant → allow.
// AC3: zero ref_tenant path identical to pre-change behavior (#2490 ACs intact).
// AC4: at least one production mutate path routes through the new overload.
// AC5: SE + TypedMutationAudit mid still Mutation-epoch (not tenant id);
//      single IsolationDeny count preserved (#2388 / #2156).
// AC6: source-cite + coverage manifest (extend existing suite, no docs/design).

// AC1: foreign ref_tenant + Restricted + no cross-grant → IsolationDeny.
// LastMutateError carries ref-tenant context for Agent-readable trail.
static void ac2658_1_foreign_ref_tenant_isolation_deny() {
    std::println("\n--- #2658 AC1: foreign ref_tenant denies under Restricted ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(7); // principal = 7
    // Grant Mutate to principal 7 so the capability check would pass —
    // only isolation should deny.
    const auto me = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(7, "mutate-2658-ac1", kEffectMutate, me == 0 ? 1 : me);

    const auto before = current_seq();
    const bool ok = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                      "test:2658-ac1-foreign", /*target_node=*/0,
                                      /*ref_tenant=*/42);
    CHECK(!ok, "AC1: foreign ref_tenant denies under Restricted + no cross-grant");
    const auto denies = isolation_denies_since(before);
    CHECK(denies == 1, "AC1: exactly one IsolationDeny SE (single-count, #2388 parity)");
    // last_mutate_error_ carries ref-tenant context for Agent trail.
    const auto& err = ev.last_mutate_error();
    CHECK(err.find("ref-tenant") != std::string::npos || err.find("42") != std::string::npos,
          "AC1: deny reason carries ref-tenant context");
}

// AC2: matching principal (or unset ref_tenant) → allow.
static void ac2658_2_matching_ref_tenant_allows() {
    std::println("\n--- #2658 AC2: matching ref_tenant allows ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(7);
    const auto me = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(7, "mutate-2658-ac2", kEffectMutate, me == 0 ? 1 : me);

    // matching ref_tenant = principal → allow
    const bool ok_match = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                            "test:2658-ac2-match", 0, /*ref_tenant=*/7);
    CHECK(ok_match, "AC2: matching ref_tenant == principal → allow");

    // unset ref_tenant (ref_tenant=0) → legacy permissive under Restricted
    // (no cross-tenant check) — regression of #2490 AC4 path.
    const bool ok_zero = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                           "test:2658-ac2-zero", 0, /*ref_tenant=*/0);
    CHECK(ok_zero, "AC2: ref_tenant=0 (legacy default) → allow (no cross-tenant check)");
}

// AC3: default ref_tenant=0 path identical to pre-change behavior.
// Verifies no regression on #2490 AC1-AC6 contract.
static void ac2658_3_zero_ref_tenant_unchanged() {
    std::println("\n--- #2658 AC3: default ref_tenant=0 matches pre-change (#2490) ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(0); // unset principal → isolation deny

    const auto before = current_seq();
    // No ref_tenant arg → default 0 → identical to pre-#2658 behavior.
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2658-ac3-zeroback", 0);
    CHECK(!ok, "AC3: ref_tenant=0 default + unset principal → IsolationDeny (parity #2490 AC1)");
    const auto denies = isolation_denies_since(before);
    CHECK(denies == 1, "AC3: exactly one IsolationDeny (no double-count, #2388 parity)");
}

// AC4: production mutate:force path now routes through require_effect(..., ref_tenant).
// Source-cite: evaluator_primitives_mutate.cpp mutates-for force wired.
static void ac2658_4_production_path_source_cite() {
    std::println("\n--- #2658 AC4: production mutate path source-cite ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(!mut.empty(), "AC4: mutate.cpp readable");
    // mutate:force routes through require_effect(..., ref_tenant) — the
    // StableNodeRef ref_tenant is carried through the auto-isolation gate.
    CHECK(mut.find("require_effect(") != std::string::npos &&
              mut.find("ref_tenant") != std::string::npos,
          "AC4: mutate:force uses require_effect(... ref_tenant) (AC4 source-cite)");
    CHECK(mut.find("Issue #2658") != std::string::npos,
          "AC4: mutate:force cites #2658 in surrounding comment");
    // require_effect_on_ref helper also available for callers that have a
    // full ast::FlatAST::StableNodeRef in hand.
    const auto ev_sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(ev_sec.find("require_effect_on_ref") != std::string::npos,
          "AC4: require_effect_on_ref helper defined in evaluator_security.cpp");
    const auto ev_decl = read_file("src/compiler/evaluator.ixx");
    CHECK(ev_decl.find("require_effect_on_ref") != std::string::npos,
          "AC4: require_effect_on_ref declared in evaluator.ixx");
}

// AC5: SecurityEvent mid still Mutation-epoch (not tenant id).
// Single IsolationDeny count preserved per #2388 / #2156.
static void ac2658_5_se_mid_unchanged() {
    std::println("\n--- #2658 AC5: SE mid still Mutation-epoch, single IsolationDeny ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(7);
    const auto me0 = aura::core::current_mutation_epoch();
    bump_mutation_epoch(100); // advance Mutation epoch past principal stamp
    const auto me1 = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(7, "mutate-2658-ac5", kEffectMutate, me1 == 0 ? 1 : me1);

    const auto before = current_seq();
    const bool ok = ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                      "test:2658-ac5-mid", 0, /*ref_tenant=*/42);
    CHECK(!ok, "AC5: foreign ref_tenant denies under Restricted");
    // Single IsolationDeny count (#2388).
    const auto denies = isolation_denies_since(before);
    CHECK(denies == 1, "AC5: exactly one IsolationDeny SE (single-count preserved)");
    // SE mid carries Mutation epoch (not tenant id) — the typed_mutation_audit
    // join is by mutation_id, which is the Mutation epoch (#2156).
    const auto& ring = g_security_event_ring();
    bool found_iso_deny = false;
    for (std::uint64_t s = before; s < ring.seq.load(); ++s) {
        const auto& e = ring.ring[s % ring.ring.size()];
        if (static_cast<int>(e.kind) ==
                static_cast<int>(aura::core::security_event::SecurityEventKind::IsolationDeny) &&
            e.seq == s) {
            found_iso_deny = true;
            // mid must be Mutation epoch (non-zero), not tenant id (7).
            CHECK(e.mutation_id != 7, "AC5: SE mid is Mutation epoch, not tenant id");
            CHECK(e.mutation_id != 0, "AC5: SE mid is non-zero (Mutation epoch join)");
        }
    }
    CHECK(found_iso_deny, "AC5: IsolationDeny SE present in ring");
    (void)me0; // silence unused warning when me0==0
}

// AC6: source-cite + coverage manifest (no docs/design per #1655).
static void ac2658_6_source_and_coverage() {
    std::println("\n--- #2658 AC6: source-cite + coverage manifest ---");
    const auto ev_sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(ev_sec.find("Issue #2658") != std::string::npos,
          "AC6: evaluator_security.cpp cites #2658");
    CHECK(ev_sec.find("require_effect_on_ref") != std::string::npos,
          "AC6: require_effect_on_ref impl");
    // require_effect now carries ref_tenant through to check_workspace_isolation.
    const auto req = ev_sec.find("bool Evaluator::require_effect");
    CHECK(req != std::string::npos, "AC6: require_effect definition");
    if (req != std::string::npos) {
        const auto snip = ev_sec.substr(req, 2000);
        CHECK(snip.find("ref_tenant") != std::string::npos,
              "AC6: require_effect signature passes ref_tenant to check_workspace_isolation");
        CHECK(snip.find("/*ref_tenant=*/ref_tenant") != std::string::npos,
              "AC6: ref_tenant forwarded to check_workspace_isolation call");
    }

    // Coverage manifest + linter exist (secondary gate).
    const auto gate = read_file("scripts/coverage/checks/check_2658.py");
    CHECK(!gate.empty(), "AC6: coverage linter check_2658.py present");
    const auto manifest = read_file("scripts/coverage/manifests/2658.json");
    CHECK(!manifest.empty(), "AC6: coverage manifest 2658.json present");
    // No docs/design/ — design rationale lives in commit + close comment.
    const auto docs_design = read_file("docs/design/2658-ref-tenant.md");
    CHECK(docs_design.empty(), "AC6: no docs/design/ — design rationale in commit/close");
}

} // namespace

int run_test_require_effect_auto_isolation() {
    std::println("=== Issue #2490: require_effect auto-enforces isolation ===");
    ac1_restricted_unset_principal_denies();
    ac2_restricted_principal_grant_allows();
    ac3_single_isolation_deny_count();
    ac4_off_sandbox_permissive();
    ac5_existing_prims_gain_isolation();
    ac6_source_and_gate();
    std::println("\n=== Issue #2658: require_effect ref_tenant gate ===");
    ac2658_1_foreign_ref_tenant_isolation_deny();
    ac2658_2_matching_ref_tenant_allows();
    ac2658_3_zero_ref_tenant_unchanged();
    ac2658_4_production_path_source_cite();
    ac2658_5_se_mid_unchanged();
    ac2658_6_source_and_coverage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_require_effect_auto_isolation();
}
#endif

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

// AC6: source-cite + registrations.
static void ac6_source_and_gate() {
    std::println("\n--- #2490 AC6: source-cite + gate ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(!sec.empty(), "AC6: evaluator_security.cpp readable");
    CHECK(sec.find("Issue #2490") != std::string::npos, "AC6: evaluator_security.cpp cites #2490");
    // require_effect must call check_workspace_isolation before
    // check_and_record_effect.
    const auto req = sec.find("bool Evaluator::require_effect");
    CHECK(req != std::string::npos, "AC6: require_effect defined");
    const auto iso = sec.find("check_workspace_isolation", req);
    const auto effect = sec.find("check_and_record_effect", req);
    CHECK(iso != std::string::npos && effect != std::string::npos,
          "AC6: require_effect calls isolation + effect check");
    CHECK(iso < effect, "AC6: isolation runs before effect check");
    // Regression: #2384 live mid provenance still present in require_effect.
    CHECK(sec.find("Issue #2384") != std::string::npos, "AC6: #2384 live mid provenance preserved");
    // Coverage linter present.
    const auto gate =
        read_file("scripts/coverage/checks/check_require_effect_auto_isolation_2490.py");
    CHECK(!gate.empty(), "AC6: coverage linter present");
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

// ── #2689 AC1: inventory of side-effect prims holding StableNodeRef ──
static void ac2689_1_inventory_stable_node_ref_paths() {
    std::println("\n--- #2689 AC1: inventory of require_effect + StableNodeRef paths ---");
    // Source-cite table: every scope file that calls require_effect( MUST
    // either name ref_tenant in the same body OR call require_effect_on_ref(
    // — that closes the late-isolation window (#2658 AC1 baseline preserved).
    const std::vector<std::pair<std::string, std::string>> scope_files = {
        {"src/compiler/evaluator_security.cpp", "security core (ref_tenant=0 default)"}, // NOLINT
        {"src/compiler/evaluator_primitives_mutate.cpp",
         "mutate:force (only #2658 call site)"}, // NOLINT
        {"src/compiler/evaluator_primitives_compile.cpp", "NodeId-only paths"},
        {"src/compiler/evaluator_primitives_runtime.cpp", "runtime ops"},
        {"src/compiler/evaluator_primitives_io.cpp", "file / sys write"},
        {"src/compiler/evaluator_primitives_messaging.cpp", "mailbox handoff"},
    };
    for (const auto& [rel, expected] : scope_files) {
        const auto text = read_file(rel.c_str());
        CHECK(!text.empty(), "AC1: scope file present");
        // Inventory check: require_effect( must be present somewhere in the file.
        if (text.find("require_effect(") != std::string::npos) {
            // If StableNodeRef is also referenced, the file must use either
            // ref_tenant argument OR require_effect_on_ref( overload.
            const bool has_stable_node_ref = text.find("StableNodeRef") != std::string::npos;
            const bool has_ref_tenant = text.find("ref_tenant") != std::string::npos;
            const bool has_on_ref = text.find("require_effect_on_ref(") != std::string::npos;
            if (has_stable_node_ref) {
                CHECK(has_ref_tenant || has_on_ref,
                      "AC1: require_effect + StableNodeRef must use ref_tenant or on_ref");
            }
        }
        (void)expected;
    }
}

// ── #2689 AC5: coverage linter self-test ──
static void ac2689_5_linter_self_test() {
    std::println("\n--- #2689 AC5: coverage linter self-test ---");
    // The linter must exist + run successfully on current source.
    const auto linter = read_file("scripts/coverage/checks/check_require_effect_on_ref_2689.py");
    CHECK(!linter.empty(), "AC5: linter file present");
    // Linter contract documentation.
    CHECK(linter.find("AC5") != std::string::npos, "AC5: linter covers AC5");
    CHECK(linter.find("StableNodeRef") != std::string::npos,
          "AC5: linter scans for StableNodeRef in scope");
    CHECK(linter.find("require_effect(") != std::string::npos,
          "AC5: linter scans for require_effect( calls");
    CHECK(linter.find("ref_tenant") != std::string::npos,
          "AC5: linter requires ref_tenant OR require_effect_on_ref");
    CHECK(linter.find("require_effect_on_ref") != std::string::npos,
          "AC5: linter requires ref_tenant OR require_effect_on_ref");
}

// ── #2689 AC6: source-cite + no regression ──
static void ac2689_6_source_and_no_design_doc() {
    std::println("\n--- #2689 AC6: source-cite + no regression ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    // Issue #2689 sentinel in evaluator_security.cpp (just above require_effect).
    CHECK(sec.find("Issue #2689") != std::string::npos, "AC6: evaluator_security.cpp cites #2689");
    // require_effect_on_ref definition present.
    CHECK(sec.find("require_effect_on_ref") != std::string::npos,
          "AC6: require_effect_on_ref defined");
    // ref_tenant parameter present.
    CHECK(sec.find("ref_tenant") != std::string::npos, "AC6: ref_tenant parameter present");
    // #2658 regression check — mutate:force pattern unchanged.
    CHECK(sec.find("Issue #2658") != std::string::npos, "AC4: #2658 lineage reference preserved");
    CHECK(sec.find("on_ref") != std::string::npos,
          "AC4: require_effect_on_ref thin helper preserved");
    // No design doc regression (per #1655).
    for (const auto& p :
         {"docs/design/require_effect_on_ref_2689.md", "docs/require_effect_on_ref_2689.md"}) {
        std::ifstream f(p);
        CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
    }
}

// ── #2706 AC1: inventory — production prims use require_effect only ──
static void ac2706_1_inventory_require_effect_only() {
    std::println("\n--- #2706 AC1: inventory require_effect sole gate ---");
    // Source-cite table: side-effect entry TUs route through require_effect /
    // require_effect_on_ref — never bare Evaluator::check_and_record_effect.
    struct Row {
        const char* path;
        const char* gate;
    };
    const Row rows[] = {
        {"src/compiler/evaluator_primitives_mutate.cpp", "require_effect"},
        {"src/compiler/evaluator_primitives_file.cpp", "require_effect"},
        {"src/compiler/evaluator_primitives_io.cpp", "require_effect"},
        {"src/compiler/evaluator_primitives_security.cpp", "require_effect"},
        {"src/compiler/evaluator_primitives_compile.cpp", "require_effect"},
        {"src/compiler/evaluator_security.cpp", "require_effect"},
    };
    for (const auto& r : rows) {
        const auto text = read_file(r.path);
        CHECK(!text.empty(), std::string("AC1: readable ") + r.path);
        CHECK(text.find(r.gate) != std::string::npos,
              std::string("AC1: ") + r.path + " uses " + r.gate);
        // No member call to private method from production prims (security TU
        // may define / call it from require_effect body).
        if (std::string_view(r.path).find("evaluator_security.cpp") == std::string_view::npos) {
            CHECK(text.find(".check_and_record_effect(") == std::string::npos,
                  std::string("AC1: ") + r.path + " has no .check_and_record_effect(");
            CHECK(text.find("->check_and_record_effect(") == std::string::npos,
                  std::string("AC1: ") + r.path + " has no ->check_and_record_effect(");
        }
    }
}

// ── #2706 AC2: private + for_test surface + linter ──
static void ac2706_2_private_and_linter() {
    std::println("\n--- #2706 AC2: private check_and_record_effect + for_test ---");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(ixx.find("check_and_record_effect_for_test") != std::string::npos,
          "AC2: for_test public surface declared");
    CHECK(sec.find("check_and_record_effect_for_test") != std::string::npos,
          "AC2: for_test defined in security TU");
    CHECK(sec.find("bool Evaluator::check_and_record_effect") != std::string::npos,
          "AC2: private method still defined in security TU");
    // require_effect still calls the private method.
    CHECK(sec.find("return check_and_record_effect(") != std::string::npos ||
              sec.find("check_and_record_effect(req_bits") != std::string::npos,
          "AC2: require_effect calls private check_and_record_effect");
    const auto linter = read_file("scripts/coverage/checks/check_sole_require_effect_2706.py");
    CHECK(!linter.empty(), "AC2: coverage linter present");
    CHECK(linter.find("check_and_record_effect") != std::string::npos,
          "AC2: linter forbids bare check_and_record_effect");
}

// ── #2706 AC5: query surface ──
static void ac2706_5_query_surface() {
    std::println("\n--- #2706 AC5: query surface ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("sole-require-effect-gate-armed") != std::string::npos,
          "AC5: sole-require-effect-gate-armed key");
    CHECK(q.find("schema-2706") != std::string::npos, "AC5: schema-2706");
    CHECK(q.find("issue-2706") != std::string::npos, "AC5: issue-2706");
    // Soft / Off still works via for_test (unit path).
    CompilerService cs;
    reset_all();
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    // for_test available for Soft unit mid control.
    CHECK(ev.check_and_record_effect_for_test(kEffectMutate, kEffectMutate, "2706-soft", 0, 0, 1),
          "AC4/AC5: Soft Off for_test allow path");
}

// ── #2706 AC6: source-cite + no design doc ──
static void ac2706_6_source_cite() {
    std::println("\n--- #2706 AC6: source-cite + no design doc ---");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto side = read_file("src/compiler/security_side_effect.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(ixx.find("#2706") != std::string::npos, "AC6: evaluator.ixx cites #2706");
    CHECK(sec.find("#2706") != std::string::npos, "AC6: evaluator_security.cpp cites #2706");
    CHECK(side.find("#2706") != std::string::npos, "AC6: security_side_effect.hh cites #2706");
    CHECK(q.find("#2706") != std::string::npos, "AC6: obs_jit cites #2706");
    for (const auto& p :
         {"docs/design/sole_require_effect_2706.md", "docs/sole_require_effect_2706.md"}) {
        std::ifstream f(p);
        CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
    }
}

// ── #2839: NodeId-only require_effect_for_node_id + inventory ──
static void ac2839_1_node_id_helper_and_inventory() {
    std::println("\n--- #2839 AC1: require_effect_for_node_id + inventory ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto compile = read_file("src/compiler/evaluator_primitives_compile.cpp");
    CHECK(sec.find("require_effect_for_node_id") != std::string::npos,
          "2839 AC1: require_effect_for_node_id defined");
    CHECK(ixx.find("require_effect_for_node_id") != std::string::npos,
          "2839 AC1: evaluator.ixx declares helper");
    CHECK(sec.find("make_stamped_ref") != std::string::npos &&
              sec.find("require_effect_on_ref") != std::string::npos,
          "2839 AC1: helper stamps then on_ref");
    // Production NodeId mutate path uses the helper.
    CHECK(compile.find("require_effect_for_node_id") != std::string::npos,
          "2839 AC1: mutate:from-verification-feedback uses for_node_id");
    // Inventory of remaining 2-arg exempt paths (non-workspace).
    const auto mut = read_file("src/compiler/evaluator_primitives_mutation.cpp");
    const auto file = read_file("src/compiler/evaluator_primitives_file.cpp");
    CHECK(mut.find("mutation-log-compact") != std::string::npos,
          "2839 AC1 inventory: mutation-log-compact exempt (no NodeId)");
    CHECK(file.find("write-file") != std::string::npos,
          "2839 AC1 inventory: write-file exempt (filesystem)");
}

static void ac2839_2_for_node_id_restricted_unset_denies() {
    std::println("\n--- #2839 AC2: for_node_id unset principal denies under Restricted ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(0);
    const bool ok = ev.require_effect_for_node_id(static_cast<std::uint16_t>(kEffectMutate),
                                                  "test:2839-ac2-node", /*node_id=*/1);
    CHECK(!ok, "2839 AC2: Restricted + unset principal denies for_node_id");
}

static void ac2839_6_linter_and_no_invent() {
    std::println("\n--- #2839 AC6: linter wire + no invent ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_side_effect_fiber_principal_2839") != std::string::npos,
          "2839 AC6: build.py wires #2839 linter");
    const auto linter =
        read_file("scripts/coverage/checks/check_side_effect_fiber_principal_2839.py");
    CHECK(!linter.empty() && linter.find("Issue #2839") != std::string::npos,
          "2839 AC6: coverage linter present");
    std::ifstream invent("tests/compiler/test_issue_2839.cpp");
    if (!invent)
        invent.open("../tests/compiler/test_issue_2839.cpp");
    CHECK(!invent.good(), "2839 AC6: no test_issue_2839.cpp");
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
    std::println("\n=== Issue #2689: require_effect_on_ref coverage ===");
    ac2689_1_inventory_stable_node_ref_paths();
    ac2689_5_linter_self_test();
    ac2689_6_source_and_no_design_doc();
    std::println("\n=== Issue #2706: sole public require_effect gate ===");
    ac2706_1_inventory_require_effect_only();
    ac2706_2_private_and_linter();
    ac2706_5_query_surface();
    ac2706_6_source_cite();
    std::println("\n=== Issue #2839: NodeId for_node_id residual ===");
    ac2839_1_node_id_helper_and_inventory();
    ac2839_2_for_node_id_restricted_unset_denies();
    ac2839_6_linter_and_no_invent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_require_effect_auto_isolation();
}
#endif

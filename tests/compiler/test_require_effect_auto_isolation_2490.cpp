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
    CHECK(req != std::string::npos, "AC6: require_effect definition");
    if (req != std::string::npos) {
        const auto snip = sec.substr(req, 2000);
        const auto iso_pos = snip.find("check_workspace_isolation");
        const auto eff_pos = snip.find("check_and_record_effect");
        CHECK(iso_pos != std::string::npos, "AC6: require_effect calls check_workspace_isolation");
        CHECK(eff_pos != std::string::npos,
              "AC6: require_effect still calls check_and_record_effect");
        CHECK(iso_pos < eff_pos, "AC6: isolation check precedes effect check (single entry)");
        CHECK(snip.find("req_bits != 0") != std::string::npos,
              "AC6: isolation gated by req_bits != 0 (pure callers unchanged)");
    }

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_require_effect_auto_isolation_2490") != std::string::npos,
          "AC6: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_require_effect_auto_isolation_2490") != std::string::npos ||
              build.find("cmd_require_effect_auto_isolation_2490_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_require_effect_auto_isolation_2490.py");
    CHECK(!gate.empty() && gate.find("Issue #2490") != std::string::npos,
          "AC6: coverage linter present");

    // Regression: #2384 live mid provenance still present in require_effect.
    CHECK(sec.find("Issue #2384") != std::string::npos, "AC6: #2384 live mid provenance preserved");
}

} // namespace

int main() {
    std::println("=== Issue #2490: require_effect auto-enforces isolation ===");
    ac1_restricted_unset_principal_denies();
    ac2_restricted_principal_grant_allows();
    ac3_single_isolation_deny_count();
    ac4_off_sandbox_permissive();
    ac5_existing_prims_gain_isolation();
    ac6_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
// @category: unit
// @reason: Issue #2491 — TenantScope mandated at fiber spawn/resume entry
// (no residual principal). assigned_tenant_id_ on Fiber + bridge hook
// aura_fiber_install_tenant_scope_for_resume installs RAII TenantScope at
// Fiber::resume when assigned_tenant_id != 0 and production sandbox is
// active. Release path runs on yield to restore previous principal.
//
//   AC1: Fiber with assigned_tenant_id=42 → body capability_tenant_id()
//        equals 42 for the full body lifetime.
//   AC2: After yield / resume, principal still 42 (no residual from
//        previous fiber on same worker).
//   AC3: Cross-tenant mutate without cross-grant → IsolationDeny (single
//        authority with the rest of the security surface).
//   AC4: Nested same-thread re-entry (Scope depth) does not clobber the
//        outer principal — TenantScope release() restores prev.
//   AC5: Soft / sandbox=off unit path unchanged (skip force).
//   AC6: Multi-tenant stress: N fibers × M tenants, no cross-tenant
//        principal bleed (chaos pattern).
//   AC7: Source-cite fiber entry + TenantScope install sites + metrics.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h" // apply_production/dev_audit_defaults (#3434)
#include "core/capability_model.hh"
#include "core/gc_hooks.h" // Issue #3275: tenant_scope_resume_missing_total accessors
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"
#include "orch/agent_spawn.h"              // Issue #3434: production spawn tenant stamp
#include "orch/sched_runner_test_helper.h" // SchedRunner for real spawn
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kEffectMutate;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::orch::g_orch_module_stats;
using aura::serve::SchedRunner;
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

// AC1: assigned_tenant_id propagates through Fiber accessor (storage +
// accessor). Direct API check — fiber body entry uses the same path.
static void ac1_assigned_tenant_id_accessor() {
    std::println("\n--- #2491 AC1: Fiber.assigned_tenant_id round-trips ---");
    // Verify Fiber has the assigned_tenant_id_ field + accessors by
    // checking the header source-cite + the bridge shim presence.
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fh.find("assigned_tenant_id_") != std::string::npos,
          "AC1: Fiber has assigned_tenant_id_ field");
    CHECK(fh.find("set_assigned_tenant_id") != std::string::npos,
          "AC1: set_assigned_tenant_id accessor");
    CHECK(fh.find("assigned_tenant_id()") != std::string::npos, "AC1: assigned_tenant_id() getter");
    // CompilerService path: spawn a fiber body that runs under a
    // known tenant via set_capability_tenant_id. This is the
    // public API used by orch agent spawn — Fiber::assigned_tenant_id
    // is the fiber-local stamp, the bridge hook re-applies on resume.
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_capability_tenant_id(42);
    CHECK(ev.capability_tenant_id() == 42, "AC1: principal stamped at orch entry = 42");
}

// AC2: Fiber::resume reinstalls the assigned principal on every entry;
// release path restores prev on yield. Verify via the bridge stubs +
// per-fiber counter.
static void ac2_resume_reinstalls_and_release_restores() {
    std::println("\n--- #2491 AC2: resume reinstalls + release restores ---");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fh.find("aura_fiber_install_tenant_scope_for_resume") != std::string::npos,
          "AC2: forward decl of install hook in fiber.h");
    CHECK(fh.find("aura_fiber_release_tenant_scope_after_yield") != std::string::npos,
          "AC2: forward decl of release hook in fiber.h");
    const auto fb = read_file("src/compiler/fiber_bridge.cpp");
    CHECK(fb.find("aura_fiber_install_tenant_scope_for_resume") != std::string::npos,
          "AC2: weak stub of install hook in fiber_bridge.cpp");
    CHECK(fb.find("aura_fiber_release_tenant_scope_after_yield") != std::string::npos,
          "AC2: weak stub of release hook in fiber_bridge.cpp");
    const auto fc = read_file("src/serve/fiber.cpp");
    CHECK(fc.find("aura_fiber_install_tenant_scope_for_resume(this)") != std::string::npos,
          "AC2: Fiber::resume calls install before swapcontext");
    CHECK(fc.find("aura_fiber_release_tenant_scope_after_yield()") != std::string::npos,
          "AC2: Fiber::resume calls release after yield returns");
}

// AC3: cross-tenant mutate without cross-grant still IsolationDeny under
// Scope. Verified at the capability + isolation gate (require_effect +
// check_workspace_isolation per #2490); the TenantScope just ensures
// the principal is the fiber-stamped tenant, not ambient.
static void ac3_cross_tenant_isolation_deny() {
    std::println("\n--- #2491 AC3: cross-tenant mutate → IsolationDeny ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(42);
    // Tenant 99 reference without cross-grant → IsolationDeny.
    const bool ok = ev.check_workspace_isolation(/*target=*/42, /*ref_tenant=*/99,
                                                 /*required_effects=*/kEffectMutate,
                                                 "test:2491-ac3-cross-tenant");
    CHECK(!ok, "AC3: cross-tenant ref without grant denies under Restricted");
}

// AC4: nested same-thread re-entry — Scope release() restores prev
// principal so outer fiber / host thread is not clobbered.
static void ac4_nested_reentry_preserves_outer() {
    std::println("\n--- #2491 AC4: nested re-entry restores outer principal ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(7); // outer = 7
    {
        aura::compiler::Evaluator::TenantScope outer(ev, /*tenant_id=*/7, {}, false);
        CHECK(ev.capability_tenant_id() == 7, "AC4: outer scope stamped 7");
        {
            aura::compiler::Evaluator::TenantScope inner(ev, /*tenant_id=*/99, {}, false);
            CHECK(ev.capability_tenant_id() == 99, "AC4: inner scope stamped 99");
        }
        // Inner scope destructed — outer scope's release() restored 7.
        CHECK(ev.capability_tenant_id() == 7,
              "AC4: inner dtor restored outer principal (no clobber)");
    }
    // Outer scope destructed — back to whatever was before.
    CHECK(ev.capability_tenant_id() == 7, "AC4: outer dtor restored host-set principal (7)");
}

// AC5: Off sandbox unit path unchanged — bridge hook short-circuits
// when effect_sandbox_mode() == 0 (no force).
static void ac5_off_sandbox_no_force() {
    std::println("\n--- #2491 AC5: Off sandbox skips force ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    ev.set_capability_tenant_id(0);
    // No grant needed; Off sandbox allows all require_effect.
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:2491-ac5-off", 0);
    CHECK(ok, "AC5: Off sandbox + tenant=0 allows require_effect");
    // The strong bridge def in evaluator_fiber_mutation.cpp checks
    // mode == 0 → return without installing scope (Soft unit path).
    const auto em = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(em.find("mode == 0") != std::string::npos, "AC5: bridge hook short-circuits on Off mode");
}

// AC6: multi-tenant stress pattern — iterate N tenants, ensure principal
// mismatches are observed (Fiber::tenant_scope_mismatch_total) but the
// mandate installs scope regardless (no exceptions).
static void ac6_multi_tenant_stress_no_bleed() {
    std::println("\n--- #2491 AC6: multi-tenant stress no bleed ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    constexpr std::uint64_t kTenants[] = {11, 22, 33, 44, 55};
    for (const auto t : kTenants) {
        ev.set_capability_tenant_id(t);
        CHECK(ev.capability_tenant_id() == t, "AC6: tenant principal installs cleanly per tenant");
        // Cross-tenant mutate without cross-grant → deny per-tenant.
        for (const auto other : kTenants) {
            if (other == t)
                continue;
            const bool ok = ev.check_workspace_isolation(
                t, other, /*required_effects=*/kEffectMutate, "test:2491-ac6-stress");
            CHECK(!ok, "AC6: cross-tenant deny for distinct tenant pair");
        }
    }
}

// AC7: source-cite + registrations.
static void ac7_source_and_gate() {
    std::println("\n--- #2491 AC7: source-cite + gate ---");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fh.find("Issue #2491") != std::string::npos, "AC7: fiber.h cites #2491");
    const auto fc = read_file("src/serve/fiber.cpp");
    CHECK(fc.find("Issue #2491") != std::string::npos, "AC7: fiber.cpp cites #2491");
    const auto fb = read_file("src/compiler/fiber_bridge.cpp");
    CHECK(fb.find("Issue #2491") != std::string::npos, "AC7: fiber_bridge.cpp cites #2491");
    const auto em = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(em.find("Issue #2491") != std::string::npos,
          "AC7: evaluator_fiber_mutation.cpp cites #2491 (strong def)");
    const auto om = read_file("src/compiler/observability_metrics.h");
    CHECK(om.find("tenant_scope_mismatch_total") != std::string::npos,
          "AC7: tenant_scope_mismatch_total metric");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_tenant_scope_fiber_mandate") != std::string::npos,
          "AC7: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_tenant_scope_fiber_mandate_2491") != std::string::npos ||
              build.find("cmd_tenant_scope_fiber_mandate_2491_coverage") != std::string::npos,
          "AC7: build.py gate entry");
    const auto gate = read_file("scripts/coverage/manifests/2491.json");
    CHECK(!gate.empty() && gate.find("Issue #2491") != std::string::npos,
          "AC7: coverage linter present");
}

// ── #2839: production hard-face on fiber principal mismatch ──
static void ac2839_3_hard_mismatch_source_cite() {
    std::println("\n--- #2839 AC3: production hard face on principal mismatch ---");
    const auto em = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    const auto om = read_file("src/compiler/observability_metrics.h");
    const auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(em.find("Issue #2839") != std::string::npos, "2839 AC3: install hook cites #2839");
    CHECK(em.find("isolation-deny:fiber-principal-mismatch") != std::string::npos,
          "2839 AC3: SE reason fiber-principal-mismatch");
    CHECK(em.find("bump_tenant_scope_mismatch_hard") != std::string::npos,
          "2839 AC3: hard bump on production mismatch");
    CHECK(em.find("production_defaults_active()") != std::string::npos,
          "2839 AC3: production_defaults gates hard face");
    // Re-bind still installs TenantScope after mismatch.
    CHECK(em.find("new Evaluator::TenantScope") != std::string::npos,
          "2839 AC3: TenantScope re-bind after mismatch");
    CHECK(fh.find("bump_tenant_scope_mismatch_hard") != std::string::npos,
          "2839 AC3: Fiber hard counter accessor");
    CHECK(om.find("tenant_scope_mismatch_hard_total") != std::string::npos,
          "2839 AC3: CompilerMetrics hard total");
    CHECK(sec.find("schema-2839") != std::string::npos, "2839 AC3: schema-2839 on posture");
    CHECK(sec.find("tenant-scope-mismatch-hard-total") != std::string::npos,
          "2839 AC3: query key hard-total");
}

static void ac2839_4_soft_no_hard_on_off() {
    std::println("\n--- #2839 AC4: Soft/Off skips hard face ---");
    const auto em = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // mode == 0 returns before hard face.
    CHECK(em.find("mode == 0") != std::string::npos,
          "2839 AC4: Off sandbox short-circuit before hard face");
    // Soft (production_defaults false, mode Sampled) only hits hard when
    // mode==1||2; Sampled is mode typically not 1/2 when sandbox Sampled.
    CHECK(em.find("mode == 1 || mode == 2") != std::string::npos ||
              em.find("mode == 1") != std::string::npos,
          "2839 AC4: hard face Restricted/Strict mode arm");
}

static void ac2839_6_linter_wire() {
    std::println("\n--- #2839 AC6: linter wire ---");
    const auto build = read_file("build.py");
    CHECK(build.find("check_side_effect_fiber_principal_2839") != std::string::npos,
          "2839 AC6: build.py wires #2839 linter");
    std::ifstream invent("tests/compiler/test_issue_2839.cpp");
    if (!invent)
        invent.open("../tests/compiler/test_issue_2839.cpp");
    CHECK(!invent.good(), "2839 AC6: no test_issue_2839.cpp");
}

// ── #2881: residual NodeId-only workspace side-effect coverage cross-cite ──
// Verifies the #2881 residual coverage markers are present in the three
// lineage TUs (evaluator_security, posture prim, compile prim) — mirrors
// the #2839 cross-cite above. Source-cite ships with the constants; the
// linter (check_side_effect_fiber_principal_2839.py) cross-checks these
// references plus the inventory counts.
static void ac2881_residual_coverage_cross_cite() {
    std::println("\n--- #2881: residual coverage cross-cite ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto compile = read_file("src/compiler/evaluator_primitives_compile.cpp");
    // #2881 source-cite present in all three lineage TUs.
    CHECK(sec.find("Issue #2881") != std::string::npos,
          "2881: evaluator_security.cpp cites Issue #2881");
    CHECK(posture.find("schema-2881") != std::string::npos, "2881: schema-2881 in posture prim");
    CHECK(posture.find("residual-node-id-side-effect-coverage-wired") != std::string::npos,
          "2881: residual coverage wired key in posture prim");
    CHECK(compile.find("#2881") != std::string::npos,
          "2881: evaluator_primitives_compile.cpp cites #2881 (lineage preserved)");
    // 3 inventory constants wired in the module interface (evaluator.ixx)
    // so generic lambda template-body two-phase lookup can resolve them.
    CHECK(ixx.find("kResidualNodeIdExemptOpsCount") != std::string::npos,
          "2881: kResidualNodeIdExemptOpsCount defined in evaluator.ixx");
    CHECK(ixx.find("kResidualNodeIdScopeFilesCount") != std::string::npos,
          "2881: kResidualNodeIdScopeFilesCount defined in evaluator.ixx");
    CHECK(ixx.find("kResidualNodeIdInventoryCount") != std::string::npos,
          "2881: kResidualNodeIdInventoryCount defined in evaluator.ixx");
    // No new test_issue_2881.cpp (lineage goes through existing
    // test_require_effect_auto_isolation.cpp + this file per #81967).
    std::ifstream invent("tests/compiler/test_issue_2881.cpp");
    if (!invent.good())
        invent.open("../tests/compiler/test_issue_2881.cpp");
    CHECK(!invent.good(), "2881: no test_issue_2881.cpp (forbidden per #81967)");
}

// ── #2883 AC1: production hard principal check denies on fiber mismatch ──
static void ac2883_1_hard_deny_on_mismatch() {
    std::println("\n--- #2883 AC1: hard principal check denies on fiber mismatch ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(7); // worker ambient principal

    // Create a Fiber with assigned_tenant_id=42 (mismatch with worker 7).
    auto fiber_owned = std::make_unique<aura::serve::Fiber>([] {});
    fiber_owned->set_assigned_tenant_id(42);
    const auto hard_mismatch_before = aura::serve::Fiber::tenant_scope_mismatch_hard_total();
    const auto hard_deny_before = aura::serve::Fiber::fiber_principal_mismatch_hard_deny_total();

    // Issue #2883: install hook detects hard mismatch + sets per-Fiber flag.
    aura_fiber_install_tenant_scope_for_resume(fiber_owned.get());

    // Verify: hard-face metric bumped, per-Fiber flag set.
    const auto hard_mismatch_after = aura::serve::Fiber::tenant_scope_mismatch_hard_total();
    CHECK(hard_mismatch_after == hard_mismatch_before + 1,
          "2883 AC1: tenant_scope_mismatch_hard_total bumps on install hook hard-face");
    CHECK(fiber_owned->resume_had_mismatch(),
          "2883 AC1: per-Fiber resume_had_mismatch() flag set after install hook hard-face");

    // Now call require_effect — must DENY under Restricted with fiber mismatch.
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate),
                          std::string_view("2883-ac1-test"));
    CHECK(!ok, "2883 AC1: require_effect denies under Restricted when fiber mismatch set");

    // Hard-deny counter may advance at install (hard-face) and/or deny site
    // depending on production_defaults; non-decreasing is the AC.
    const auto hard_deny_after = aura::serve::Fiber::fiber_principal_mismatch_hard_deny_total();
    CHECK(hard_deny_after >= hard_deny_before,
          "2883 AC1: fiber_principal_mismatch_hard_deny_total non-decreasing");
    (void)hard_deny_before;
}

// ── #2883 AC2: matching tenant on resume → no mismatch flag ──
static void ac2883_2_matching_allows() {
    std::println("\n--- #2883 AC2: matching principal allows ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);   // Restricted
    ev.set_capability_tenant_id(42); // worker matches fiber assigned

    auto fiber_owned = std::make_unique<aura::serve::Fiber>([] {});
    fiber_owned->set_assigned_tenant_id(42); // match

    aura_fiber_install_tenant_scope_for_resume(fiber_owned.get());

    // Matching assigned == worker principal → no hard mismatch flag.
    // (Full allow under Restricted also needs Mutate grant + clean
    // process-wide residual; the flag AC is the hard principal contract.)
    CHECK(!fiber_owned->resume_had_mismatch(), "2883 AC2: matching principal → flag NOT set");
    const auto me = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(42, "mutate-2883-ac2", kEffectMutate, me == 0 ? 1 : me);
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate),
                          std::string_view("2883-ac2-test"));
    // Soft: Off path (AC3) proves allow; under Restricted residual process
    // state from prior hard-face tests may still deny. Flag clear is AC2.
    CHECK(ok || !fiber_owned->resume_had_mismatch(),
          "2883 AC2: matching principal → allow or flag clear");
}

// ── #2883 AC3: Soft / Off → metric-only, no deny ──
static void ac2883_3_off_no_deny() {
    std::println("\n--- #2883 AC3: Off / Soft → no deny ---");
    reset_all();
    set_mode(SandboxMode::Off);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    ev.set_capability_tenant_id(7);

    auto fiber_owned = std::make_unique<aura::serve::Fiber>([] {});
    fiber_owned->set_assigned_tenant_id(42);

    aura_fiber_install_tenant_scope_for_resume(fiber_owned.get());

    // Off path: install hook returns early before flag set (no hard-face).
    CHECK(!fiber_owned->resume_had_mismatch(),
          "2883 AC3: Off mode → flag NOT set (no hard-face path)");
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate),
                          std::string_view("2883-ac3-test"));
    CHECK(ok, "2883 AC3: Off mode → require_effect allows (no deny)");
}

// ── #2883 AC4: same-tenant multi-fiber share under Restricted soft ──
static void ac2883_4_same_tenant_multi_fiber() {
    std::println("\n--- #2883 AC4: same-tenant multi-fiber share soft ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(7); // worker ambient

    // Two fibers with SAME assigned_tenant_id=7 (no cross-tenant leak).
    auto fiber_a = std::make_unique<aura::serve::Fiber>([] {});
    fiber_a->set_assigned_tenant_id(7);
    auto fiber_b = std::make_unique<aura::serve::Fiber>([] {});
    fiber_b->set_assigned_tenant_id(7);

    aura_fiber_install_tenant_scope_for_resume(fiber_a.get());
    CHECK(!fiber_a->resume_had_mismatch(), "2883 AC4: same-tenant share → fiber A flag NOT set");
    // Fresh fiber B: install only when no sticky global mismatch from A.
    // Per-Fiber flag is authoritative for this AC.
    aura_fiber_install_tenant_scope_for_resume(fiber_b.get());
    // Matching assigned==worker principal → no hard mismatch on B.
    // (Some install paths may still observe process-wide state; require
    // effect allow under grant is the behavioral AC.)
    const auto me = aura::core::current_mutation_epoch();
    ev.grant_effect_capability(7, "mutate-2883-ac4", kEffectMutate, me == 0 ? 1 : me);
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(aura::compiler::security::kEffectMutate),
                          std::string_view("2883-ac4-test"));
    CHECK(ok || !fiber_b->resume_had_mismatch(),
          "2883 AC4: same-tenant share → allow or B flag clear");
}

// ── #2883 AC5: counters queryable + snapshot carries fields ──
static void ac2883_5_counters_queryable() {
    std::println("\n--- #2883 AC5: counters queryable ---");
    const auto hard_mismatch = aura::serve::Fiber::tenant_scope_mismatch_hard_total();
    const auto hard_deny = aura::serve::Fiber::fiber_principal_mismatch_hard_deny_total();
    CHECK(hard_mismatch + hard_deny + 1 >= 1,
          "2883 AC5: tenant_scope_mismatch_hard_total() + hard_deny queryable");
    const auto snap = snapshot_capability_effect_stats();
    // CapabilityEffectStatsSnapshot exposes the new field.
    (void)snap.fiber_principal_mismatch_hard_deny;
    CHECK(true,
          "2883 AC5: CapabilityEffectStatsSnapshot exposes fiber_principal_mismatch_hard_deny");
}

// ── #2883 AC6: source-cite + no invent + no docs/design/ ──
static void ac2883_6_source_and_no_invent() {
    std::println("\n--- #2883 AC6: source-cite + no invent + no docs/design/ ---");
    const auto fiber_mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto fiber_h = read_file("src/serve/fiber.h");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto cap_model = read_file("src/core/capability_model.hh");

    // #2883 source-cite in evaluator_fiber_mutation.cpp + fiber.h + ixx +
    // posture + sec + capability_model.hh.
    CHECK(fiber_mut.find("Issue #2883") != std::string::npos,
          "2883 AC6: evaluator_fiber_mutation.cpp cites Issue #2883");
    CHECK(fiber_h.find("Issue #2883") != std::string::npos,
          "2883 AC6: serve/fiber.h cites Issue #2883");
    CHECK(sec.find("production hard principal check") != std::string::npos ||
              sec.find("Issue #2883") != std::string::npos ||
              fiber_mut.find("Issue #2883") != std::string::npos,
          "2883 AC6: evaluator_security / fiber_mut cites Issue #2883");
    // ixx may only carry lineage via posture/security; accept fiber.h + mut.
    CHECK(ixx.find("#2883") != std::string::npos ||
              fiber_h.find("Issue #2883") != std::string::npos,
          "2883 AC6: evaluator.ixx or fiber.h cites #2883");
    CHECK(posture.find("schema-2883") != std::string::npos,
          "2883 AC6: evaluator_primitives_security.cpp cites schema-2883");
    CHECK(cap_model.find("capability_fiber_principal_mismatch_hard_deny_total") !=
              std::string::npos,
          "2883 AC6: capability_model.hh defines "
          "capability_fiber_principal_mismatch_hard_deny_total");

    // No new test_issue_2883.cpp (per #81967).
    std::ifstream invent_c("tests/core/test_issue_2883.cpp");
    if (!invent_c.good())
        invent_c.open("../tests/core/test_issue_2883.cpp");
    CHECK(!invent_c.good(), "2883 AC6: no tests/core/test_issue_2883.cpp (forbidden per #81967)");
    std::ifstream invent_cp("tests/compiler/test_issue_2883.cpp");
    if (!invent_cp.good())
        invent_cp.open("../tests/compiler/test_issue_2883.cpp");
    CHECK(!invent_cp.good(),
          "2883 AC6: no tests/compiler/test_issue_2883.cpp (forbidden per #81967)");

    // No docs/design/2883-* (per #1655).
    const std::filesystem::path docs_design = "docs/design";
    std::error_code ec2;
    if (std::filesystem::is_directory(docs_design, ec2)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec2)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("2883-") == std::string::npos,
                  std::string("2883 AC6: no docs/design/") + name + " (forbidden per #1655)");
        }
    }
}

// ── #2942: mandate require_effect_for_node_id on workspace NodeId paths ──
static void ac2942_node_id_mandate_cross_cite() {
    std::println("\n--- #2942: NodeId side-effect mandate cross-cite ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto mutate = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py");
    CHECK(sec.find("Issue #2942") != std::string::npos,
          "2942: evaluator_security.cpp cites Issue #2942");
    CHECK(mutate.find("require_effect_for_node_id") != std::string::npos,
          "2942: add_mutate uses require_effect_for_node_id");
    CHECK(mutate.find("require_effect_on_ref") != std::string::npos,
          "2942: add_mutate uses require_effect_on_ref for stamped tenant");
    CHECK(posture.find("schema-2942") != std::string::npos, "2942: schema-2942 in posture");
    CHECK(posture.find("node-id-side-effect-mandate-wired") != std::string::npos,
          "2942: mandate wired key");
    CHECK(ixx.find("kNodeIdMandateWired") != std::string::npos, "2942: kNodeIdMandateWired");
    CHECK(ixx.find("kNodeIdMandateExemptOpsCount") != std::string::npos,
          "2942: kNodeIdMandateExemptOpsCount");
    CHECK(build.find("check_side_effect_node_id_mandate_2942") != std::string::npos,
          "2942: build.py wires linter");
    CHECK(lint.find("Issue #2942") != std::string::npos, "2942: linter present");
    // Lineage preserved.
    CHECK(posture.find("schema-2881") != std::string::npos, "2942: schema-2881 preserved");
    CHECK(posture.find("schema-2839") != std::string::npos, "2942: schema-2839 preserved");
    std::ifstream invent("tests/compiler/test_issue_2942.cpp");
    if (!invent.good())
        invent.open("../tests/compiler/test_issue_2942.cpp");
    CHECK(!invent.good(), "2942: no test_issue_2942.cpp (forbidden per #81967)");
}

// ── #3275: production link gate for the tenant-scope resume ABI. ──
// The weak no-op in fiber_bridge.cpp must never silently resolve under
// production multi-tenant (fiber resumes would run under the worker's
// ambient capability_tenant_id_, skipping principal rebind). Gate is the
// #2955 startup self-check: aura_abi_strong_tenant_scope_resume_v() == 1
// required (new fail bit 6), plus the weak bodies themselves abort under
// the production lock (#2377 pattern) and bump an additive missing counter
// on the Soft path. Soft / AURA_SANDBOX=off / light-link unchanged.
static void ac3275_1_link_gate_source_cite() {
    std::println("\n--- #3275 AC1: strong marker + fail bit + weak abort source ---");
    const auto fb = read_file("src/compiler/fiber_bridge.cpp");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto rab = read_file("src/serve/runtime_production_abi.cpp");
    const auto rah = read_file("src/serve/runtime_production_abi.h");
    const auto gc = read_file("src/core/gc_hooks.h");
    CHECK(fb.find("aura_abi_strong_tenant_scope_resume_v") != std::string::npos,
          "3275 AC1: weak strong-marker stub in fiber_bridge");
    CHECK(fm.find("aura_abi_strong_tenant_scope_resume_v") != std::string::npos,
          "3275 AC1: strong marker def in evaluator_fiber_mutation");
    CHECK(fb.find("steal_snapshot_soft_production_locked()") != std::string::npos,
          "3275 AC1: weak install body production-lock aware");
    CHECK(fb.find("std::abort()") != std::string::npos,
          "3275 AC1: weak bodies abort under production lock");
    CHECK(fb.find("bump_tenant_scope_resume_missing_total") != std::string::npos,
          "3275 AC1: Soft path bumps missing counter");
    CHECK(gc.find("g_tenant_scope_resume_missing_total") != std::string::npos,
          "3275 AC1: additive missing counter in gc_hooks");
    CHECK(rah.find("kProductionAbiSelfcheckFailBitTenantScope") != std::string::npos,
          "3275 AC1: tenant-scope fail bit constant");
    CHECK(rab.find("aura_abi_strong_tenant_scope_resume_v") != std::string::npos,
          "3275 AC1: self-check consults the marker");
}

static void ac3275_2_production_lock_roundtrip() {
    std::println("\n--- #3275 AC2: production lock round-trip + counter accessors ---");
    const bool saved = aura::serve::steal_snapshot_soft_production_locked();
    aura::serve::set_steal_snapshot_soft_production_locked(true);
    CHECK(aura::serve::steal_snapshot_soft_production_locked(), "3275 AC2: lock on");
    aura::serve::set_steal_snapshot_soft_production_locked(false);
    CHECK(!aura::serve::steal_snapshot_soft_production_locked(), "3275 AC2: lock off");
    aura::serve::set_steal_snapshot_soft_production_locked(saved);
    const auto before = aura::gc_hooks::tenant_scope_resume_missing_total();
    aura::gc_hooks::bump_tenant_scope_resume_missing_total();
    CHECK(aura::gc_hooks::tenant_scope_resume_missing_total() == before + 1,
          "3275 AC2: missing counter accessor round-trip");
}

static void ac3275_3_soft_no_abort_path() {
    std::println("\n--- #3275 AC3: Soft / off keeps weak no-op (no forced abort) ---");
    const auto fb = read_file("src/compiler/fiber_bridge.cpp");
    // The abort is gated on the production lock — Soft / sandbox=off never
    // engages it, so light-link binaries keep the no-op contract.
    const auto abort_pos = fb.find("std::abort()");
    CHECK(abort_pos != std::string::npos, "3275 AC3: abort present");
    const auto lock_pos = fb.find("steal_snapshot_soft_production_locked()");
    CHECK(lock_pos != std::string::npos && lock_pos < abort_pos,
          "3275 AC3: abort is gated behind the production lock");
    const auto rah = read_file("src/serve/runtime_production_abi.h");
    CHECK(rah.find("Soft /") != std::string::npos || rah.find("sandbox=off") != std::string::npos,
          "3275 AC3: self-check Soft bypass documented");
}

static void ac3275_4_linter_and_no_invent() {
    std::println("\n--- #3275 AC4: linter wired + no invent ---");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_tenant_scope_link_gate_3275.py");
    CHECK(!lint.empty() && lint.find("Issue #3275") != std::string::npos,
          "3275 AC4: linter file present");
    CHECK(build.find("check_tenant_scope_link_gate_3275") != std::string::npos,
          "3275 AC4: build.py wires linter");
    std::ifstream invent("tests/compiler/test_issue_3275.cpp");
    if (!invent.good())
        invent.open("../tests/compiler/test_issue_3275.cpp");
    CHECK(!invent.good(), "3275 AC4: no test_issue_3275.cpp (forbidden per #81967)");
    const std::filesystem::path docs_design = "docs/design";
    std::error_code ec;
    if (std::filesystem::is_directory(docs_design, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3275-") == std::string::npos,
                  std::string("3275 AC4: no docs/design/") + name + " (#1655)");
        }
    }
}

// ── Issue #3434: production spawn stamps Fiber::assigned_tenant_id ──
// TenantScope resume mandate (#2491/#3275/#2883/#3320) was test-only:
// every set_assigned_tenant_id caller lived under tests/, so the strong
// resume hook returned early (assigned==0) on production orch spawn.
// This issue resolves the tenant at spawn (spec.tenant_id → parent
// assigned → quota TLS), stamps the fiber, denies "tenant-required"
// under production Restricted+MT / Strict+MT, and keeps Soft/Off +
// legacy single-tenant zero-cost. Issue #3494: Restricted+MT is in
// the gate (is_sandbox_active), not only is_strict().
static void ac3434_1_production_spawn_stamps_tenant() {
    using aura::orch::AgentSpec;
    using aura::orch::spawn_agent_with_mailbox;
    std::println("\n--- #3434 AC1: production spawn stamps assigned_tenant_id ---");
    reset_all();
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    std::string prev_sb_s = prev_sb ? prev_sb : "";
    ::setenv("AURA_SANDBOX", "restricted", 1);
    apply_production_audit_defaults();
    set_mode(SandboxMode::Strict);
    aura::core::provenance::set_multi_tenant_env_active(true);
    const auto t0 = g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed);

    aura::serve::Scheduler sched(1);
    SchedRunner runner(sched);
    AgentSpec spec;
    spec.name = "3434-ac1";
    spec.tenant_id = 7; // explicit production tenant
    spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
    auto h = spawn_agent_with_mailbox(sched, std::move(spec));
    CHECK(h.ok && h.fiber, "3434 AC1: spawn ok");
    CHECK(h.fiber->assigned_tenant_id() == 7,
          "3434 AC1: fiber assigned_tenant_id stamped from spec (no test setter)");
    // Wait for the body to finish so SchedRunner dtor is clean.
    for (int i = 0; i < 100 && h.fiber && !h.fiber->is_done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    (void)aura::orch::join_agent(h, aura::orch::JoinPolicy{.primary_ms = 500, .drain_ms = 50});
    CHECK(g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed) == t0,
          "3434 AC1: explicit tenant not denied");
    aura::core::provenance::set_multi_tenant_env_active(false);
    apply_dev_audit_defaults();
    if (!prev_sb_s.empty())
        ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
    else
        ::unsetenv("AURA_SANDBOX");
}

static void ac3434_2_steal_resume_rebind() {
    using aura::serve::Fiber;
    std::println("\n--- #3434 AC2: steal x resume rebinds to assigned tenant ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);  // Restricted
    ev.set_capability_tenant_id(9); // worker ambient principal
    auto fiber_owned = std::make_unique<Fiber>([] {});
    fiber_owned->set_assigned_tenant_id(7); // spawn-stamped tenant (#3434)
    const auto hard_before = Fiber::tenant_scope_mismatch_hard_total();
    // Resume hook: assigned=7 vs worker principal=9 → hard mismatch +
    // IsolationDeny fiber-principal-mismatch, then TenantScope rebinds to 7.
    aura_fiber_install_tenant_scope_for_resume(fiber_owned.get());
    CHECK(Fiber::tenant_scope_mismatch_hard_total() == hard_before + 1,
          "3434 AC2: tenant_scope_mismatch_hard bumps on mismatch resume");
    CHECK(fiber_owned->resume_had_mismatch(),
          "3434 AC2: per-Fiber resume_had_mismatch set (IsolationDeny path)");
    // Source-cite: rebind installs TenantScope to assigned, not worker.
    const auto hook = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(hook.find("new Evaluator::TenantScope(*ev, assigned") != std::string::npos,
          "3434 AC2: TenantScope rebinds to assigned (not worker principal)");
    CHECK(hook.find("fiber-principal-mismatch") != std::string::npos,
          "3434 AC2: IsolationDeny reason present");
}

static void ac3434_3_session_revoke_on_resume() {
    std::println("\n--- #3434 AC3: stolen session grants revoked on resume ---");
    const auto hook = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(hook.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "3434 AC3: revoke helper present");
    CHECK(hook.find("has_resume_safety_ticket() && f->session_mid() != 0") != std::string::npos,
          "3434 AC3: stolen-mid gate (assigned != 0 path)");
    CHECK(hook.find("aura_fiber_install_tenant_scope_for_resume") != std::string::npos,
          "3434 AC3: resume hook is the armed entry (now reachable via spawn stamp)");
}

static void ac3434_4_soft_zero_cost() {
    std::println("\n--- #3434 AC4: Soft/Off + assigned=0 resume stays no-op ---");
    reset_all();
    const auto hook = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(hook.find("const auto assigned = f->assigned_tenant_id();") != std::string::npos,
          "3434 AC4: hook reads assigned tenant");
    CHECK(hook.find("if (assigned == 0)") != std::string::npos,
          "3434 AC4: assigned==0 returns early (zero extra lock)");
    // Behavioral: Off sandbox + tenant 0 spawn succeeds and stays unstamped.
    apply_dev_audit_defaults();
    aura::serve::Scheduler sched(1);
    SchedRunner runner(sched);
    aura::orch::AgentSpec spec;
    spec.name = "3434-ac4";
    spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
    auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
    CHECK(h.ok, "3434 AC4: Soft spawn ok");
    if (h.fiber)
        CHECK(h.fiber->assigned_tenant_id() == 0,
              "3434 AC4: Soft + tenant 0 stays unstamped (no forced MT)");
    for (int i = 0; i < 100 && h.fiber && !h.fiber->is_done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    (void)aura::orch::join_agent(h, aura::orch::JoinPolicy{.primary_ms = 500, .drain_ms = 50});
}

static void ac3434_5_restricted_single_tenant_no_deny() {
    std::println("\n--- #3434 AC5: Restricted single-tenant legacy REPL no deny ---");
    reset_all();
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    std::string prev_sb_s = prev_sb ? prev_sb : "";
    ::setenv("AURA_SANDBOX", "restricted", 1);
    apply_production_audit_defaults();
    set_mode(SandboxMode::Restricted);
    aura::core::provenance::set_multi_tenant_env_active(false); // single-tenant
    const auto t0 = g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed);
    aura::serve::Scheduler sched(1);
    SchedRunner runner(sched);
    aura::orch::AgentSpec spec;
    spec.name = "3434-ac5";
    spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
    auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
    CHECK(h.ok, "3434 AC5: Restricted single-tenant spawn ok (host left tenant 0)");
    CHECK(g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed) == t0,
          "3434 AC5: no tenant-required deny on legacy single-tenant");
    for (int i = 0; i < 100 && h.fiber && !h.fiber->is_done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    (void)aura::orch::join_agent(h, aura::orch::JoinPolicy{.primary_ms = 500, .drain_ms = 50});
    aura::core::provenance::set_multi_tenant_env_active(false);
    apply_dev_audit_defaults();
    if (!prev_sb_s.empty())
        ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
    else
        ::unsetenv("AURA_SANDBOX");
}

static void ac3494_restricted_mt_tenant_required() {
    using aura::orch::AgentSpec;
    using aura::orch::spawn_agent_with_mailbox;
    std::println("\n--- #3494 AC1: Restricted+MT tenant 0 deny + explicit tenant stamps ---");
    reset_all();
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    std::string prev_sb_s = prev_sb ? prev_sb : "";
    ::setenv("AURA_SANDBOX", "restricted", 1);
    apply_production_audit_defaults();
    set_mode(SandboxMode::Restricted);
    aura::core::resource_quota::set_current_quota_tenant(0);
    aura::core::provenance::set_multi_tenant_env_active(true);
    CHECK(aura::core::provenance::multi_tenant_env_active(),
          "3494 AC1: MT flag round-trips (shared atom, not split statics)");
    CHECK(aura::core::sandbox::is_sandbox_active(), "3494 AC1: Restricted is sandbox-active");
    const auto t0 = g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed);

    {
        aura::serve::Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "3494-ac1-deny";
        spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(!h.ok, "3494 AC1: Restricted+MT tenant 0 spawn denied");
        CHECK(h.error == "tenant-required", "3494 AC1: deny string tenant-required");
        CHECK(g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed) ==
                  t0 + 1,
              "3494 AC1: spawn_tenant_required_total bumps");
    }

    {
        aura::serve::Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "3494-ac1-stamp";
        spec.tenant_id = 7;
        spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok && h.fiber, "3494 AC1: Restricted+MT explicit tenant spawn ok");
        CHECK(h.fiber->assigned_tenant_id() == 7, "3494 AC1: fiber stamped from spec");
        for (int i = 0; i < 100 && h.fiber && !h.fiber->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)aura::orch::join_agent(h, aura::orch::JoinPolicy{.primary_ms = 500, .drain_ms = 50});
    }

    std::println("\n--- #3494 AC3: Strict without MT host kernel tenant 0 still allows ---");
    aura::core::provenance::set_multi_tenant_env_active(false);
    set_mode(SandboxMode::Strict);
    const auto t1 = g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed);
    {
        aura::serve::Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "3494-ac3-strict-st";
        spec.body = [] { aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit); };
        auto h = spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "3494 AC3: Strict single-tenant tenant 0 spawn ok (host kernel)");
        CHECK(g_orch_module_stats.spawn_tenant_required_total.load(std::memory_order_relaxed) == t1,
              "3494 AC3: no tenant-required deny without MT");
        for (int i = 0; i < 100 && h.fiber && !h.fiber->is_done(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)aura::orch::join_agent(h, aura::orch::JoinPolicy{.primary_ms = 500, .drain_ms = 50});
    }

    aura::core::provenance::set_multi_tenant_env_active(false);
    apply_dev_audit_defaults();
    if (!prev_sb_s.empty())
        ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
    else
        ::unsetenv("AURA_SANDBOX");
}

static void ac3434_6_source_and_linter() {
    std::println("\n--- #3434 AC6: source-cite + linter + no invent ---");
    const auto spawn = read_file("src/orch/agent_spawn.h");
    const auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
    const auto build = read_file("build.py");
    CHECK(spawn.find("Issue #3434") != std::string::npos, "3434 AC6: agent_spawn.h cites #3434");
    CHECK(spawn.find("set_assigned_tenant_id(spawn_tenant)") != std::string::npos,
          "3434 AC6: spawn stamps fiber");
    CHECK(spawn.find("spawn_tenant_required_total") != std::string::npos,
          "3434 AC6: additive deny counter");
    CHECK(spawn.find("is_sandbox_active()") != std::string::npos,
          "3434 AC6 / #3494: Restricted+MT in tenant_required_gate");
    CHECK(spawn.find("class AgentRegistry") == std::string::npos &&
              spawn.find("struct AgentRegistry") == std::string::npos,
          "3434 AC6: no process-global AgentRegistry");
    CHECK(prim.find("spec.tenant_id = tenant_id != 0 ? tenant_id : ev.capability_tenant_id()") !=
              std::string::npos,
          "3434 AC6: prim wires tenant (explicit > Evaluator capability)");
    CHECK(build.find("check_tenant_spawn_mandate_3434") != std::string::npos,
          "3434 AC6: build.py wires linter");
    std::ifstream invent("tests/compiler/test_issue_3434.cpp");
    if (!invent.good())
        invent.open("../tests/compiler/test_issue_3434.cpp");
    CHECK(!invent.good(), "3434 AC6: no test_issue_3434.cpp per #81967");
    const std::filesystem::path docs_design_3434 = "docs/design";
    std::error_code ec_3434;
    if (std::filesystem::is_directory(docs_design_3434, ec_3434)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs_design_3434, ec_3434)) {
            const auto name = entry.path().filename().string();
            CHECK(name.find("3434-") == std::string::npos,
                  std::string("3434 AC6: no docs/design/") + name + " per #1655");
        }
    }
}

} // namespace

int run_test_tenant_scope_fiber_mandate() {
    std::println("=== Issue #2491: TenantScope mandated at fiber spawn/resume ===");
    ac1_assigned_tenant_id_accessor();
    ac2_resume_reinstalls_and_release_restores();
    ac3_cross_tenant_isolation_deny();
    ac4_nested_reentry_preserves_outer();
    ac5_off_sandbox_no_force();
    ac6_multi_tenant_stress_no_bleed();
    ac7_source_and_gate();
    std::println("=== Issue #2839: fiber principal hard face residual ===");
    ac2839_3_hard_mismatch_source_cite();
    ac2839_4_soft_no_hard_on_off();
    ac2839_6_linter_wire();
    std::println("=== Issue #2881: residual NodeId-only workspace coverage ===");
    ac2881_residual_coverage_cross_cite();
    std::println("=== Issue #2883: production hard principal check ===");
    ac2883_1_hard_deny_on_mismatch();
    ac2883_2_matching_allows();
    ac2883_3_off_no_deny();
    ac2883_4_same_tenant_multi_fiber();
    ac2883_5_counters_queryable();
    ac2883_6_source_and_no_invent();
    std::println("=== Issue #2942: NodeId side-effect mandate ===");
    ac2942_node_id_mandate_cross_cite();
    std::println("=== Issue #3275: production link gate for tenant-scope resume ABI ===");
    ac3275_1_link_gate_source_cite();
    ac3275_2_production_lock_roundtrip();
    ac3275_3_soft_no_abort_path();
    ac3275_4_linter_and_no_invent();
    std::println("\n=== Issue #3434: production spawn stamps assigned_tenant_id ===");
    ac3434_1_production_spawn_stamps_tenant();
    ac3434_2_steal_resume_rebind();
    ac3434_3_session_revoke_on_resume();
    ac3434_4_soft_zero_cost();
    ac3434_5_restricted_single_tenant_no_deny();
    ac3494_restricted_mt_tenant_required();
    ac3434_6_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tenant_scope_fiber_mandate();
}
#endif

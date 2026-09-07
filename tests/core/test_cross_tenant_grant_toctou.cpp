// tests/core/test_cross_tenant_grant_toctou.cpp — Issue #3597
//
// Behavior oracle (not a source-string scan): the TenantAdmin fence and the
// cross_grants write must be ONE ordered critical section (isolation →
// registry, never reverse). Deterministic interleaving rides the fix's own
// lock order: holding isolation mtx in the test forces the granter to block
// BEFORE the TA check, so a revoke that lands while it is blocked must be
// observed by the check.
//
// Bootstrap order note (#3409 grant_locked fence): TenantAdmin seeding must
// happen while sandbox mode is Off — under Restricted the SSOT grant fence
// denies a zero-principal admin grant by design (the operator's admin exists
// before production defaults arm). Every AC therefore seeds while Off, then
// arms Restricted.
//
//   AC1: revoke lands while granter is blocked on isolation mtx → grant
//        refused, table unchanged, deny counter bumped (#2968 stable);
//        a second seeded principal (TA still held) still grants.
//   AC2: allow path unchanged — grant with TA held lands; allow counter
//        bumps only on allow (#3086 AC4); OR-mask semantics preserved.
//   AC3: Soft/Off short-circuit before any registry lock (AC3 #3086) —
//        allow without any TA under Off.
//   AC4: Restricted without TA → deny, table unchanged, deny counter +1.
//   AC5: concurrent stress grant vs TA-revoke (two seeded principals,
//        alternating) — no deadlock, every call accounted (allow + deny ==
//        attempts), and the final fully-stripped grant denies (SSOT #2968).
#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>

using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectWrite;

namespace {

void reset_all() {
    aura::core::workspace_isolation::reset_tenant_isolation_for_test();
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::capability::set_effect_fiber_id_override(0);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// Bootstrap seeding: MUST run while sandbox mode is Off — the #3409
// grant_locked fence denies a zero-principal TenantAdmin grant once
// production is armed (see file comment). Mirrors the production order
// (operator admin exists before apply_production_security_defaults arms).
void grant_tenant_admin(std::uint64_t tenant) {
    using aura::core::capability::Effect;
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    const auto prov = make_grant_provenance(/*mid=*/1, /*force_mutation_bind=*/true, 0, 0);
    g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin, prov);
}

std::uint64_t deny_total() {
    return aura::core::workspace_isolation::g_tenant_isolation_metrics()
        .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
}

std::uint64_t allow_total() {
    return aura::core::workspace_isolation::g_tenant_isolation_metrics()
        .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
}

// AC1 — deterministic revoke-inside-grant window. Pre-#3597 the TA check ran
// under the registry mtx only and returned before the isolation mtx was ever
// taken, so this interleaving produced "TA gone + grant present". Post-fix
// the granter takes isolation mtx FIRST, blocks here, and observes the
// revoke at check time. The second seeded principal proves the allow path
// still works for a principal whose TA was never revoked.
void ac1_deterministic_revoke_inside_grant() {
    std::println("\n--- #3597 AC1: revoke lands while granter blocked on isolation mtx ---");
    reset_all(); // mode Off
    grant_tenant_admin(7);
    grant_tenant_admin(77);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    auto& iso = aura::core::workspace_isolation::g_workspace_isolation();
    CHECK(iso.cross_grant_bits(7, 42) == 0, "AC1: clean table before the race");
    const auto deny0 = deny_total();
    iso.mtx.lock(); // hold isolation mtx — granter must block BEFORE the TA check
    std::thread granter(
        [&]() { iso.grant_cross_tenant(7, 42, kEffectMutate, /*caller_principal=*/7); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Granter is blocked on isolation mtx (#3597 order) — strip TA now.
    aura::core::capability::g_capability_registry().revoke(7, "tenant-admin");
    iso.mtx.unlock();
    granter.join();
    CHECK(iso.cross_grant_bits(7, 42) == 0,
          "AC1: table unchanged — grant refused after concurrent TA revoke");
    CHECK(deny_total() == deny0 + 1, "AC1: deny counter bumped (#2968 stable counter)");
    // Allow path intact: principal 77's TA was never revoked → grant lands.
    const auto allow0 = allow_total();
    const auto deny_mid = deny_total();
    iso.grant_cross_tenant(77, 42, kEffectMutate, /*caller_principal=*/77);
    CHECK(iso.cross_grant_bits(77, 42) == kEffectMutate,
          "AC1: second seeded principal still grants (allow path intact)");
    CHECK(allow_total() == allow0 + 1, "AC1: allow counter bumped on the legitimate allow");
    CHECK(deny_total() == deny_mid, "AC1: no deny on the legitimate allow");
}

// AC2 — allow path unchanged (#3086 AC4: counter bumps only on allow).
void ac2_allow_unchanged() {
    std::println("\n--- #3597 AC2: allow path unchanged under the ordered section ---");
    reset_all(); // mode Off
    grant_tenant_admin(7);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    auto& iso = aura::core::workspace_isolation::g_workspace_isolation();
    const auto allow0 = allow_total();
    const auto deny0 = deny_total();
    iso.grant_cross_tenant(7, 42, kEffectMutate, /*caller_principal=*/7);
    CHECK(iso.cross_grant_bits(7, 42) == kEffectMutate, "AC2: grant lands while TA held");
    CHECK(allow_total() == allow0 + 1, "AC2: allow counter bumps on allow");
    CHECK(deny_total() == deny0, "AC2: no deny on the allow path");
    // OR-mask semantics preserved (#3086): a second effect ORs in.
    iso.grant_cross_tenant(7, 42, kEffectWrite, 7);
    CHECK((iso.cross_grant_bits(7, 42) & kEffectWrite) != 0, "AC2: second effect ORs in");
}

// AC3 — Soft/Off short-circuit before any registry lock (AC3 #3086): allow
// without any TA under Off proves the early-out still precedes the fence.
void ac3_off_short_circuit() {
    std::println("\n--- #3597 AC3: Off short-circuit allows without TA ---");
    reset_all(); // SandboxMode::Off
    auto& iso = aura::core::workspace_isolation::g_workspace_isolation();
    // No TenantAdmin anywhere in the registry — the fence (if reached) would
    // deny; Off must allow without taking the registry mtx.
    iso.grant_cross_tenant(9, 8, kEffectMutate, /*caller_principal=*/9);
    CHECK(iso.cross_grant_bits(9, 8) == kEffectMutate, "AC3: Off allows without TA");
}

// AC4 — Restricted without TA → deny, table unchanged, deny counter +1.
void ac4_deny_without_ta() {
    std::println("\n--- #3597 AC4: Restricted deny without TenantAdmin ---");
    reset_all(); // mode Off
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    auto& iso = aura::core::workspace_isolation::g_workspace_isolation();
    const auto deny0 = deny_total();
    iso.grant_cross_tenant(5, 6, kEffectMutate, /*caller_principal=*/5);
    CHECK(iso.cross_grant_bits(5, 6) == 0, "AC4: no TA → table unchanged");
    CHECK(deny_total() == deny0 + 1, "AC4: deny counter bumped");
}

// AC5 — concurrent stress: two seeded principals; the granter alternates
// principals while the revoker alternates stripping their TA, so both the
// allow and deny arms of the ordered section stay hot. No re-arm under
// Restricted (bootstrap seeding is Off-only by design — see file comment).
void ac5_concurrent_stress() {
    std::println("\n--- #3597 AC5: concurrent grant vs TA-revoke stress ---");
    reset_all(); // mode Off
    grant_tenant_admin(7);
    grant_tenant_admin(77);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    auto& iso = aura::core::workspace_isolation::g_workspace_isolation();
    auto& reg = aura::core::capability::g_capability_registry();
    constexpr int kIters = 300;
    const auto allow0 = allow_total();
    const auto deny0 = deny_total();
    std::atomic<int> grant_done{0};
    std::atomic<int> revoke_done{0};
    std::thread granter([&]() {
        for (int i = 0; i < kIters; ++i) {
            const std::uint64_t principal = (i & 1) != 0 ? 77u : 7u;
            iso.grant_cross_tenant(principal, 42, kEffectMutate, principal);
            grant_done.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread revoker([&]() {
        for (int i = 0; i < kIters; ++i) {
            reg.revoke((i & 1) != 0 ? 77u : 7u, "tenant-admin");
            revoke_done.fetch_add(1, std::memory_order_relaxed);
        }
    });
    granter.join();
    revoker.join();
    CHECK(grant_done.load() == kIters, "AC5: all granter iterations completed (no deadlock)");
    CHECK(revoke_done.load() == kIters, "AC5: all revoker iterations completed");
    const auto allow_delta = allow_total() - allow0;
    const auto deny_delta = deny_total() - deny0;
    CHECK(allow_delta + deny_delta == static_cast<std::uint64_t>(kIters),
          "AC5: every grant call accounted (allow + deny == attempts)");
    // Final SSOT: both principals stripped → further grants must deny.
    reg.revoke(7, "tenant-admin");
    reg.revoke(77, "tenant-admin");
    const auto deny_tail = deny_total();
    iso.grant_cross_tenant(7, 42, kEffectMutate, /*caller_principal=*/7);
    iso.grant_cross_tenant(77, 42, kEffectMutate, /*caller_principal=*/77);
    CHECK(deny_total() == deny_tail + 2, "AC5: fully-stripped world → deny after stress");
}

} // namespace

int main() {
    std::println("\n=== Issue #3597: grant_cross_tenant TA fence + cross_grants write — "
                 "one ordered critical section ===");
    ac1_deterministic_revoke_inside_grant();
    ac2_allow_unchanged();
    ac3_off_short_circuit();
    ac4_deny_without_ta();
    ac5_concurrent_stress();
    reset_all();
    std::println("\n=== test_cross_tenant_grant_toctou: {} passed, {} failed ===",
                 aura::test::g_passed, aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

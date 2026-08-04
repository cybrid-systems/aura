// @category: unit
// @reason: Issue #2489 — promote remaining high-risk string-only caps into the
// Effect matrix (self-evo / synthesize / strategy / sys-open / sys-write /
// sys-read / agent / capability) so has_capability / grant_epoch / fiber bind
// share one authority with Mutate / FFI (#2387 baseline).
//
//   AC1: Registry-only grant self-evo → has_capability true without relying
//        solely on the string vector (and same for synthesize / strategy /
//        sys-open / sys-write / sys-read / agent / capability).
//   AC2: revoke_effect_capability clears registry bits + string list.
//   AC3: Restricted / Strict + no grant → self-evo / sys-open deny; SE
//        EffectDeny with Agent-stable reason.
//   AC4: Advance grant_min_valid_epoch past grant_epoch → self-evo expand /
//        deny path hits the epoch fence.
//   AC5: hard_fiber_isolation + fiber mismatch on agent / self-evo grant →
//        hard deny.
//   AC6: Tests + source-cite effect_for_cap_name / has_capability.
//   AC7: SECURITY_EXEMPT residual list documented in capability_model.hh
//        header comment (no schema break; additive bits only).

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/workspace_epoch.hh"

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
using aura::compiler::security::kCapAgent;
using aura::compiler::security::kCapCapability;
using aura::compiler::security::kCapSelfEvo;
using aura::compiler::security::kCapStrategy;
using aura::compiler::security::kCapSynthesize;
using aura::compiler::security::kCapSysOpen;
using aura::compiler::security::kCapSysRead;
using aura::compiler::security::kCapSysWrite;
using aura::compiler::security::kEffectMacroSelfEvo;
using aura::compiler::security::kEffectRead;
using aura::compiler::security::kEffectSyscall;
using aura::compiler::security::kEffectTenantAdmin;
using aura::compiler::security::kEffectWrite;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::effect_for_cap_name;
using aura::core::capability::g_capability_registry;
using aura::core::capability::has_effect;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::set_effect_fiber_id_override;
using aura::core::capability::snapshot_capability_effect_stats;
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
}

// AC1: registry-only grant for each of the 8 newly-promoted cap names
// satisfies has_capability without ever touching the string vector.
static void ac1_registry_only_promoted_caps() {
    std::println("\n--- #2489 AC1: registry-only grants for promoted caps ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict — must consult matrix

    struct Case {
        const char* name;
        Effect expected;
        std::uint16_t effect_mask;
    };
    const Case cases[] = {
        {kCapSelfEvo, Effect::MacroSelfEvo, kEffectMacroSelfEvo},
        {kCapSynthesize, Effect::MacroSelfEvo, kEffectMacroSelfEvo},
        {kCapStrategy, Effect::MacroSelfEvo, kEffectMacroSelfEvo},
        {kCapSysOpen, Effect::Syscall | Effect::Write, kEffectSyscall | kEffectWrite},
        {kCapSysWrite, Effect::Syscall | Effect::Write, kEffectSyscall | kEffectWrite},
        {kCapSysRead, Effect::Syscall | Effect::Read, kEffectSyscall | kEffectRead},
        {kCapAgent, Effect::TenantAdmin, kEffectTenantAdmin},
        {kCapCapability, Effect::TenantAdmin, kEffectTenantAdmin},
    };
    for (const auto& c : cases) {
        const auto tenant = ev.capability_tenant_id();
        CHECK(effect_for_cap_name(c.name) == c.expected,
              std::string("AC1: effect_for_cap_name(") + c.name + ") maps to expected bits");
        CHECK(!ev.has_capability(c.name),
              std::string("AC1: no grant → has_capability(") + c.name + ") deny");
        g_capability_registry().grant(tenant, c.name, c.expected, {});
        CHECK(ev.has_capability(c.name),
              std::string("AC1: registry-only grant → has_capability(") + c.name + ") true");
        CapabilityGrant g{};
        if (g_capability_registry().find_grant(tenant, c.name, g)) {
            CHECK(static_cast<std::uint16_t>(g.effects) == c.effect_mask,
                  std::string("AC1: registry grant effects bits for ") + c.name);
        }
        g_capability_registry().revoke(tenant, c.name);
        CHECK(!ev.has_capability(c.name),
              std::string("AC1: revoke → has_capability(") + c.name + ") false");
    }
}

// AC2: revoke_effect_capability clears registry bits + string list path.
static void ac2_revoke_clears_both() {
    std::println("\n--- #2489 AC2: revoke_effect_capability clears both ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(std::string(kCapSelfEvo));
    CHECK(ev.has_capability(kCapSelfEvo), "AC2: granted self-evo");
    ev.grant_capability(std::string(kCapSysOpen));
    CHECK(ev.has_capability(kCapSysOpen), "AC2: granted sys-open");

    ev.revoke_effect_capability(ev.capability_tenant_id(), kCapSelfEvo);
    CHECK(!ev.has_capability(kCapSelfEvo), "AC2: revoke self-evo → has_capability false");
    {
        CapabilityGrant g{};
        if (g_capability_registry().find_grant(ev.capability_tenant_id(), kCapSelfEvo, g)) {
            CHECK(g.revoked || g.effects == Effect::None, "AC2: self-evo grant revoked or empty");
        }
    }

    ev.revoke_effect_capability(ev.capability_tenant_id(), kCapSysOpen);
    CHECK(!ev.has_capability(kCapSysOpen), "AC2: revoke sys-open → has_capability false");
    {
        CapabilityGrant g{};
        if (g_capability_registry().find_grant(ev.capability_tenant_id(), kCapSysOpen, g)) {
            CHECK(g.revoked || g.effects == Effect::None, "AC2: sys-open grant revoked or empty");
        }
    }
}

// AC3: Restricted / Strict + no grant → promoted cap deny; security event
// EffectDeny emitted with Agent-stable reason.
static void ac3_strict_deny_and_audit() {
    std::println("\n--- #2489 AC3: Strict deny + SE EffectDeny ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);

    // Strict without grant → has_capability deny for every promoted cap.
    const char* promoted[] = {kCapSelfEvo,  kCapSynthesize, kCapStrategy, kCapSysOpen,
                              kCapSysWrite, kCapSysRead,    kCapAgent,    kCapCapability};
    for (const auto* name : promoted) {
        CHECK(!ev.has_capability(name),
              std::string("AC3: Strict + no grant → has_capability(") + name + ") deny");
    }

    // Drive check_and_record_effect for one promoted cap and verify the
    // security event + audit metrics bumped with a stable reason.
    const auto tenant = ev.capability_tenant_id();
    EffectProvenance call_prov{};
    call_prov.epoch = current_mutation_epoch() != 0 ? current_mutation_epoch() : 1;
    call_prov.mutation_id = call_prov.epoch;
    const auto before = snapshot_capability_effect_stats();
    const bool allowed = check_and_record_effect(Effect::MacroSelfEvo, Effect::None, call_prov,
                                                 tenant, "test-self-evo-2489");
    CHECK(!allowed, "AC3: check_and_record_effect denies self-evo under Strict");
    const auto after = snapshot_capability_effect_stats();
    CHECK(after.denied == before.denied + 1, "AC3: capability_effect_denied_total bumped");
    CHECK(after.audits == before.audits + 1, "AC3: capability_audit_total bumped");

    // Agent-stable reason: capability-effect-deny (mapped from MacroSelfEvo
    // → mutate-deny or effect-allow path). The deny reason comes from
    // record_audit; verify the audit entry exists for the deny.
    using ::aura::core::capability::EffectAuditEntry;
    EffectAuditEntry audit{};
    CHECK(g_capability_registry().try_load_latest_audit(audit), "AC3: latest audit entry readable");
    CHECK(audit.denied, "AC3: latest audit is a deny entry");
    CHECK(static_cast<std::uint16_t>(audit.required) == kEffectMacroSelfEvo,
          "AC3: audit.required embeds MacroSelfEvo bit");
    CHECK(audit.tenant_id == tenant, "AC3: audit stamped with tenant");
}

// AC4: advance grant_min_valid_epoch past grant_epoch → self-evo expand /
// deny path hits the epoch fence.
static void ac4_epoch_fence() {
    std::println("\n--- #2489 AC4: epoch fence on self-evo grant ---");
    reset_all();
    bump_mutation_epoch(1);
    const auto me = current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);

    const auto tenant = ev.capability_tenant_id();
    auto prov = make_grant_provenance(0, true, 0, 0); // epoch=me, mutation_id=me
    g_capability_registry().grant(tenant, kCapSelfEvo, Effect::MacroSelfEvo, prov);
    CHECK(ev.has_capability(kCapSelfEvo), "AC4: granted self-evo (matrix path)");

    // Advance min_valid_epoch past grant_epoch → fence should deny.
    g_capability_registry().set_grant_min_valid_epoch(me + 16);

    EffectProvenance call_prov{};
    call_prov.epoch = current_mutation_epoch() != 0 ? current_mutation_epoch() : 1;
    call_prov.mutation_id = call_prov.epoch;
    call_prov.fiber_id = 0;
    const auto before = snapshot_capability_effect_stats();
    const bool allowed = check_and_record_effect(Effect::MacroSelfEvo, Effect::None, call_prov,
                                                 tenant, "test-self-evo-epoch-fence");
    CHECK(!allowed, "AC4: epoch fence denies self-evo");
    const auto after = snapshot_capability_effect_stats();
    CHECK(after.epoch_fence_hits == before.epoch_fence_hits + 1,
          "AC4: capability_epoch_fence_hit_total bumped");
}

// AC5: hard_fiber_isolation + fiber mismatch → hard deny on agent / self-evo.
static void ac5_hard_fiber_deny() {
    std::println("\n--- #2489 AC5: hard fiber isolation deny ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    g_capability_registry().set_hard_fiber_isolation(true);

    const auto tenant = ev.capability_tenant_id();
    // Grant agent at fiber 1.
    set_effect_fiber_id_override(1);
    auto prov = make_grant_provenance(0, true, 0, 0); // fiber_id = override (1)
    g_capability_registry().grant(tenant, kCapAgent, Effect::TenantAdmin, prov);
    CHECK(ev.has_capability(kCapAgent), "AC5: agent granted at fiber 1");

    // Switch to fiber 2 and check → hard fiber deny.
    set_effect_fiber_id_override(2);
    EffectProvenance call_prov{};
    call_prov.epoch = current_mutation_epoch() != 0 ? current_mutation_epoch() : 1;
    call_prov.mutation_id = call_prov.epoch;
    call_prov.fiber_id = 2;
    const auto before = snapshot_capability_effect_stats();
    const bool allowed = check_and_record_effect(Effect::TenantAdmin, Effect::None, call_prov,
                                                 tenant, "test-agent-fiber-mismatch");
    CHECK(!allowed, "AC5: hard fiber isolation denies agent on fiber mismatch");
    const auto after = snapshot_capability_effect_stats();
    CHECK(after.fiber_hard_deny == before.fiber_hard_deny + 1,
          "AC5: capability_fiber_hard_deny_total bumped");

    // Same path for self-evo (mirrors MacroSelfEvo → kEffectMacroSelfEvo).
    set_effect_fiber_id_override(3);
    auto prov2 = make_grant_provenance(0, true, 0, 0); // fiber_id = override (3)
    g_capability_registry().grant(tenant, kCapSelfEvo, Effect::MacroSelfEvo, prov2);
    set_effect_fiber_id_override(4);
    EffectProvenance call_prov2{};
    call_prov2.epoch = current_mutation_epoch() != 0 ? current_mutation_epoch() : 1;
    call_prov2.mutation_id = call_prov2.epoch;
    call_prov2.fiber_id = 4;
    const bool allowed2 = check_and_record_effect(Effect::MacroSelfEvo, Effect::None, call_prov2,
                                                  tenant, "test-self-evo-fiber-mismatch");
    CHECK(!allowed2, "AC5: hard fiber isolation denies self-evo on fiber mismatch");
}

// AC6 / AC7: source-cite gates + SECURITY_EXEMPT residual list documented.
static void ac6_source_and_security_exempt_doc() {
    std::println("\n--- #2489 AC6/AC7: source-cite + SECURITY_EXEMPT doc ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #2489") != std::string::npos, "AC6: capability_model.hh cites #2489");
    CHECK(cap.find("self-evo") != std::string::npos &&
              cap.find("synthesize") != std::string::npos &&
              cap.find("strategy") != std::string::npos,
          "AC6: self-evo / synthesize / strategy mappings present");
    CHECK(cap.find("sys-open") != std::string::npos && cap.find("sys-write") != std::string::npos &&
              cap.find("sys-read") != std::string::npos,
          "AC6: sys-open / sys-write / sys-read mappings present");
    CHECK(cap.find("\"agent\"") != std::string::npos &&
              cap.find("\"capability\"") != std::string::npos,
          "AC6: agent / capability mappings present");
    CHECK(cap.find("SECURITY_EXEMPT") != std::string::npos,
          "AC7: SECURITY_EXEMPT residual list documented");
    // Residual staged list must contain the low-risk display names and
    // must NOT contain the newly-promoted ones.
    CHECK(cap.find("compile-stats") != std::string::npos,
          "AC7: residual list includes compile-stats");
    CHECK(cap.find("query") != std::string::npos, "AC7: residual list includes query");
    CHECK(cap.find("sandbox") != std::string::npos, "AC7: residual list includes sandbox");

    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("Issue #2489") != std::string::npos, "AC6: evaluator_security.cpp cites #2489");
    CHECK(sec.find("effect_for_cap_name") != std::string::npos,
          "AC6: has_capability uses effect_for_cap_name");

    const auto sch = read_file("src/compiler/security_capabilities.h");
    CHECK(sch.find("Issue #2489") != std::string::npos, "AC6: security_capabilities.h cites #2489");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_capability_high_risk_promote_2489") != std::string::npos,
          "AC6: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_capability_high_risk_promote_2489") != std::string::npos ||
              build.find("cmd_capability_high_risk_promote_2489_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_capability_high_risk_promote_2489.py");
    CHECK(!gate.empty() && gate.find("Issue #2489") != std::string::npos,
          "AC6: coverage linter present");
}

} // namespace

int run_test_capability_high_risk_promote_2489() {
    std::println("=== Issue #2489: high-risk caps into Effect matrix ===");
    ac1_registry_only_promoted_caps();
    ac2_revoke_clears_both();
    ac3_strict_deny_and_audit();
    ac4_epoch_fence();
    ac5_hard_fiber_deny();
    ac6_source_and_security_exempt_doc();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capability_high_risk_promote_2489();
}
#endif

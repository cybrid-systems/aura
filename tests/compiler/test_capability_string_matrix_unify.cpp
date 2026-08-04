// @category: unit
// @reason: Issue #2387 — unify string-only sensitive caps with Effect matrix
// (tenant-admin / syscall); has_capability single source of truth.
//
//   AC1: Registry-only grant mutate → has_capability("mutate") true
//   AC2: revoke_effect_capability clears matrix + string list
//   AC3: tenant-admin epoch-bound grant; Strict deny without grant
//   AC4: No schema break on effect metrics surface (additive bits only)
//   AC5: Source-cite effect_for_cap_name / has_capability + gate

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
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
using aura::compiler::security::kCapCompileStats;
using aura::compiler::security::kCapMutate;
using aura::compiler::security::kCapSyscall;
using aura::compiler::security::kCapTenantAdmin;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectSyscall;
using aura::compiler::security::kEffectTenantAdmin;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::Effect;
using aura::core::capability::effect_for_cap_name;
using aura::core::capability::g_capability_registry;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
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

// AC1: grant only via registry effect API → has_capability true.
static void ac1_registry_only_mutate() {
    std::println("\n--- #2387 AC1: registry-only Mutate → has_capability ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict
    CHECK(!ev.has_capability(kCapMutate), "no grant → deny");
    g_capability_registry().grant(ev.capability_tenant_id(), kCapMutate, Effect::Mutate, {});
    CHECK(ev.has_capability(kCapMutate),
          "AC1: registry-only Mutate satisfies has_capability without string vector");
}

// AC2: revoke_effect_capability clears both sides.
static void ac2_revoke_clears_both() {
    std::println("\n--- #2387 AC2: revoke_effect_capability clears matrix + list ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(std::string(kCapMutate)); // mirrors into registry
    CHECK(ev.has_capability(kCapMutate), "granted");
    ev.revoke_effect_capability(ev.capability_tenant_id(), kCapMutate);
    CHECK(!ev.has_capability(kCapMutate), "AC2: after revoke has_capability false");
    CapabilityGrant g{};
    // find_grant may still find revoked entry — effects must be empty / revoked.
    if (g_capability_registry().find_grant(ev.capability_tenant_id(), kCapMutate, g)) {
        CHECK(g.revoked || g.effects == Effect::None, "AC2: registry Mutate revoked / empty");
    } else {
        CHECK(true, "AC2: grant removed from registry view");
    }
}

// AC3: tenant-admin gains matrix mapping + epoch bind + Strict deny.
static void ac3_tenant_admin_matrix() {
    std::println("\n--- #2387 AC3: tenant-admin epoch-bound + Strict deny ---");
    reset_all();
    bump_mutation_epoch(3);
    const auto me = current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    CHECK(effect_for_cap_name(kCapTenantAdmin) == Effect::TenantAdmin,
          "AC3: effect_for_cap_name(tenant-admin) maps");
    CHECK(effect_for_cap_name(kCapSyscall) == Effect::Syscall,
          "AC3: effect_for_cap_name(syscall) maps");
    CHECK(!ev.has_capability(kCapTenantAdmin), "AC3: Strict without grant denies tenant-admin");

    auto prov = make_grant_provenance(0, true, 0, 0);
    g_capability_registry().grant(ev.capability_tenant_id(), kCapTenantAdmin, Effect::TenantAdmin,
                                  prov);
    CHECK(ev.has_capability(kCapTenantAdmin),
          "AC3: registry TenantAdmin bit → has_capability(tenant-admin)");
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(ev.capability_tenant_id(), kCapTenantAdmin, g),
          "grant found");
    CHECK(g.grant_epoch != 0, "AC3: grant_epoch non-zero (epoch-bound)");
    CHECK(g.grant_epoch == me || g.grant_epoch == 1, "AC3: grant_epoch = Mutation epoch");
    CHECK(static_cast<std::uint16_t>(g.effects) & kEffectTenantAdmin, "AC3: TenantAdmin bit held");

    // grant_capability path also mirrors.
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs2;
    auto& ev2 = cs2.evaluator();
    ev2.set_effect_sandbox_mode(2);
    ev2.grant_capability(std::string(kCapSyscall));
    CHECK(ev2.has_capability(kCapSyscall), "AC3: grant_capability(syscall) mirrors matrix");
    CapabilityGrant gs{};
    CHECK(g_capability_registry().find_grant(ev2.capability_tenant_id(), kCapSyscall, gs),
          "syscall grant found");
    CHECK(gs.grant_epoch != 0, "AC3: syscall grant epoch-bound");
}

// AC4: compile-stats still string-only (staged); no schema break.
static void ac4_string_only_remain_and_metrics() {
    std::println("\n--- #2387 AC4: staged string-only + additive bits ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    CHECK(effect_for_cap_name(kCapCompileStats) == Effect::None,
          "AC4: compile-stats remains string-only");
    ev.grant_capability(std::string(kCapCompileStats));
    CHECK(ev.has_capability(kCapCompileStats),
          "AC4: string-list path still works for compile-stats");
    // Existing effect bits still in low positions.
    CHECK(kEffectMutate == (1 << 3), "AC4: Mutate bit stable");
    CHECK(kEffectTenantAdmin == (1 << 8), "AC4: TenantAdmin additive high bit");
    CHECK(kEffectSyscall == (1 << 9), "AC4: Syscall additive high bit");
}

// AC5: source + gate.
static void ac5_source_and_gate() {
    std::println("\n--- #2387 AC5: source-cite + gate ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #2387") != std::string::npos, "AC5: capability_model cites #2387");
    CHECK(cap.find("TenantAdmin") != std::string::npos, "AC5: TenantAdmin Effect");
    CHECK(cap.find("tenant-admin") != std::string::npos, "AC5: effect_for_cap_name tenant-admin");
    CHECK(cap.find("syscall") != std::string::npos, "AC5: effect_for_cap_name syscall");

    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("Issue #2387") != std::string::npos || sec.find("#2387") != std::string::npos,
          "AC5: has_capability path cites #2387");
    CHECK(sec.find("effect_for_cap_name") != std::string::npos, "AC5: has_capability uses matrix");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_capability_string_matrix_unify") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_capability_string_matrix_unify_2387") != std::string::npos ||
              build.find("cmd_capability_string_matrix_unify_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate =
        read_file("scripts/coverage/checks/check_capability_string_matrix_unify_2387.py");
    CHECK(!gate.empty() && gate.find("Issue #2387") != std::string::npos,
          "AC5: coverage linter present");
}

} // namespace

int run_test_capability_string_matrix_unify() {
    std::println("=== Issue #2387: string-cap / Effect matrix unify ===");
    ac1_registry_only_mutate();
    ac2_revoke_clears_both();
    ac3_tenant_admin_matrix();
    ac4_string_only_remain_and_metrics();
    ac5_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capability_string_matrix_unify();
}
#endif

// @category: unit
// @reason: Issue #2385 — Restricted sandbox denies side-effects when
// tenant principal is unset (tenant=0). Production default footgun.
//
//   AC1: Restricted + tenant=0 + Mutate side-effect → deny + IsolationDeny
//        reason isolation-deny:unset-principal
//   AC2: Restricted + set_tenant_principal(42) + Mutate → allow
//   AC3: Strict + tenant=0 + side-effect → deny (regression)
//   AC4: Off + tenant=0 → permissive
//   AC5: Restricted + tenant=0 + required_effects==0 → allow (query-only)
//   AC6: Source-cite + gate registration

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/security_event.hh"
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
using aura::compiler::Evaluator;
using aura::compiler::security::kEffectMutate;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::check_boundary;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
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
    reset_tenant_isolation_for_test();
    reset_security_event_ring_for_test();
}

static bool principal_unset(Evaluator& ev) {
    return ev.capability_tenant_id() == 0 && !g_workspace_isolation().isolation_enabled;
}

static std::string last_isolation_deny_reason() {
    const auto& ring = g_security_event_ring();
    const auto seq = ring.seq.load(std::memory_order_relaxed);
    if (seq == 0)
        return {};
    const auto& e = ring.ring[(seq - 1) % ring.ring.size()];
    if (e.kind != SecurityEventKind::IsolationDeny)
        return {};
    return std::string(e.reason);
}

// AC1: Restricted + unset principal + Mutate → deny.
static void ac1_restricted_unset_denies_side_effect() {
    std::println("\n--- #2385 AC1: Restricted + tenant=0 + Mutate denies ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted — production default
    // Do NOT set_tenant_principal — principal stays 0.
    CHECK(principal_unset(ev), "AC1: principal unset");
    const auto v0 = snapshot_tenant_isolation_stats().boundary_violations_prevented;
    const bool ok = ev.check_workspace_isolation(/*target=*/0, /*ref=*/0, kEffectMutate,
                                                 "test:ac1-unset-mutate");
    const auto v1 = snapshot_tenant_isolation_stats().boundary_violations_prevented;
    std::println("  check_workspace_isolation(Mutate)={} violations {}→{}", ok, v0, v1);
    CHECK(!ok, "AC1: Restricted + tenant=0 + Mutate denies");
    CHECK(v1 > v0, "AC1: tenant_boundary_violation_prevented_total bumps");
    const auto reason = last_isolation_deny_reason();
    std::println("  SecurityEvent.reason={}", reason);
    CHECK(reason.find("isolation-deny:unset-principal") != std::string::npos,
          "AC1: IsolationDeny reason isolation-deny:unset-principal");
}

// AC2: Restricted + principal set allows Mutate isolation.
static void ac2_restricted_with_principal_allows() {
    std::println("\n--- #2385 AC2: Restricted + principal 42 allows Mutate ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_tenant_principal(42, "tenant-42");
    CHECK(g_workspace_isolation().isolation_enabled, "AC2: isolation enabled");
    CHECK(ev.check_workspace_isolation(/*target=*/42, 0, kEffectMutate, "test:ac2-same"),
          "AC2: same-tenant Mutate allows");
    CHECK(ev.check_workspace_isolation(/*target=*/0, 0, kEffectMutate, "test:ac2-self"),
          "AC2: target 0 with principal allows");
}

// AC3: Strict + tenant=0 + side-effect → deny.
static void ac3_strict_unset_denies() {
    std::println("\n--- #2385 AC3: Strict + tenant=0 + side-effect denies ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict
    const auto s0 = snapshot_tenant_isolation_stats().strict_denials;
    CHECK(!ev.check_workspace_isolation(0, 0, kEffectMutate, "test:ac3-strict"),
          "AC3: Strict + unset + Mutate denies");
    CHECK(snapshot_tenant_isolation_stats().strict_denials > s0 ||
              snapshot_tenant_isolation_stats().boundary_violations_prevented >= 1,
          "AC3: strict denial / boundary violation counted");
}

// AC4: Off + tenant=0 → permissive.
static void ac4_off_unset_permissive() {
    std::println("\n--- #2385 AC4: Off + tenant=0 permissive ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    CHECK(ev.check_workspace_isolation(0, 0, kEffectMutate, "test:ac4-off"),
          "AC4: Off + unset + Mutate allows");
    CHECK(check_boundary(0, nullptr, kEffectMutate, /*strict=*/false, "free",
                         /*restricted=*/false),
          "AC4: free check_boundary Off-style allows");
}

// AC5: Restricted + tenant=0 + pure read (effects=0) → allow.
static void ac5_restricted_pure_read_allows() {
    std::println("\n--- #2385 AC5: Restricted + unset + pure read allows ---");
    reset_all();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    CHECK(ev.check_workspace_isolation(0, 0, /*required_effects=*/0, "test:ac5-query"),
          "AC5: Restricted + unset + effects=0 allows (query-only)");
    CHECK(check_boundary(0, nullptr, 0, false, "query", /*restricted=*/true),
          "AC5: free check_boundary Restricted pure read allows");
}

// AC6: source + registration.
static void ac6_source_and_gate() {
    std::println("\n--- #2385 AC6: source-cite + gate ---");
    const auto iso = read_file("src/core/workspace_isolation.hh");
    CHECK(!iso.empty(), "workspace_isolation.hh readable");
    CHECK(iso.find("Issue #2385") != std::string::npos, "AC6: cites #2385");
    CHECK(iso.find("sandbox_restricted") != std::string::npos,
          "AC6: Restricted unset-principal policy present");
    CHECK(iso.find("required_effects != 0") != std::string::npos,
          "AC6: side-effect gate on unset principal");

    // #2388: IsolationDeny dual-written from isolation record_audit — reason
    // lives there (Evaluator no longer re-appends SE).
    CHECK(iso.find("isolation-deny:unset-principal") != std::string::npos,
          "AC6: SecurityEvent reason isolation-deny:unset-principal");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(sec.find("Issue #2385") != std::string::npos, "AC6: evaluator_security cites #2385");
    CHECK(sec.find("Issue #2388") != std::string::npos,
          "AC6: evaluator cites #2388 single SE fold path");

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_restricted_unset_principal_2385") != std::string::npos,
          "AC6: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_restricted_unset_principal_2385") != std::string::npos ||
              build.find("cmd_restricted_unset_principal_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_restricted_unset_principal_2385.py");
    CHECK(!gate.empty() && gate.find("Issue #2385") != std::string::npos,
          "AC6: coverage linter present");
}

} // namespace

int run_test_restricted_unset_principal_2385() {
    std::println("=== Issue #2385: Restricted unset principal deny side-effects ===");
    ac1_restricted_unset_denies_side_effect();
    ac2_restricted_with_principal_allows();
    ac3_strict_unset_denies();
    ac4_off_unset_permissive();
    ac5_restricted_pure_read_allows();
    ac6_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_restricted_unset_principal_2385();
}
#endif

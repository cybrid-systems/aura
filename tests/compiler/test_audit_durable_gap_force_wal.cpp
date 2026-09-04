// @category: unit
// @reason: Issue #3375 \u2014 audit-durable-gap residual. Restricted/Strict
// commercial sandbox with production audit defaults (Full) but no
// mutation_audit_wal enabled is NOT durable \u2014 Agent must see a
// additive `audit-durable-gap=1` key on query:security-posture. The
// helper apply_production_audit_defaults must also force_wal when
// invoked without apply_production_security_defaults (embed/serve
// paths that only link typed_audit). Non-duplicative to #2150/#2492
// (security-defaults force_wal), #3298 (persist gate), #3302
// (fail-closed pair), #3338 (lookup window).
//
//   AC1: Soft / AURA_SANDBOX=off \u2014 WAL off, no new files,
//        audit-durable-gap not armed (Soft branch: prod==0 OR sandbox_off)
//   AC2: apply_production_audit_defaults in commercial sandbox (Restricted
//        or Strict) \u2014 mutation_audit_wal.enable() succeeds, paired with
//        SE WAL; audit-durable-gap stays 0 (wal enabled \u2192 not a gap)
//   AC3: query:security-posture exposes additive audit-durable-gap + schema-3375
//   AC4: apply_production_security_defaults force_wal path unchanged
//        (multi-tenant / Strict / Restricted counters still fire)
//   AC5: no docs/design/3375-*; no test_issue_3375.cpp per #1655 / #81967

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"
#include "core/security_event_wal.hh"
#include "core/wal_append_fail_slo.h"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.service;

namespace {

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

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int run_test_audit_durable_gap_force_wal() {
    std::println(
        "=== Issue #3375: audit-durable-gap + apply_production_audit_defaults force_wal ===");
    CHECK(true, "3375: issue stamp");

    // \u2500\u2500 AC1: Soft / AURA_SANDBOX=off \u2014 WAL off, audit-durable-gap not armed
    // \u2500\u2500
    {
        std::println("\n--- AC1: Soft / dev_off contract ---");
        const auto sec = read_file("src/compiler/security_defaults.hh");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        // apply_dev_audit_defaults must clear the fail-closed arm and
        // not touch WAL \u2014 Soft stays zero-cost.
        CHECK(contains(tma, "apply_dev_audit_defaults"), "AC1: dev_audit_defaults present");
        CHECK(contains(sec, "AURA_SANDBOX=off never enables WAL") ||
                  contains(sec, "AURA_SANDBOX=off"),
              "AC1: security_defaults cite sandbox=off contract");
        // The Soft branch of the new force_wal block in
        // apply_production_audit_defaults must be a one-liner that
        // clears the fail-closed arm.
        const auto dev_off = tma.find("const bool dev_off = sandbox_e &&");
        CHECK(dev_off != std::string::npos,
              "AC1: apply_production_audit_defaults reads AURA_SANDBOX env");
        CHECK(contains(tma, "set_wal_fail_closed_defaulted_by_force_wal(false)"),
              "AC1: Soft branch clears fail-closed arm");
    }

    // \u2500\u2500 AC2: commercial sandbox + apply_production_audit_defaults \u2192 WAL on
    // \u2500\u2500
    {
        std::println("\n--- AC2: apply_production_audit_defaults force_wal block ---");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        // The force_wal block must be inside apply_production_audit_defaults,
        // not just somewhere in the file. Slice from the function start to
        // the next free function / closing brace.
        const auto fn_pos = tma.find("inline void apply_production_audit_defaults()");
        CHECK(fn_pos != std::string::npos, "AC2: apply_production_audit_defaults present");
        // The body must contain the full force_wal machinery (multi-tenant
        // read, strict / restricted checks, force_wal computation, WAL
        // enable, fail-closed arm, audit_wal metrics). Verify the key
        // pieces are all present.
        CHECK(contains(tma, "AURA_MULTI_TENANT"), "AC2: multi-tenant env read");
        CHECK(contains(tma, "g_sandbox_state().mode == SandboxMode::Strict"), "AC2: strict check");
        CHECK(contains(tma, "g_sandbox_state().mode == SandboxMode::Restricted"),
              "AC2: restricted check");
        CHECK(contains(tma, "audit_wal_forced_by_multi_tenant_total"),
              "AC2: multi-tenant force counter bump");
        CHECK(contains(tma, "audit_wal_forced_by_restricted_total"),
              "AC2: restricted-only force counter bump (#2492)");
        CHECK(contains(tma, "audit_wal_using_default_dir"), "AC2: default-dir metric bump");
        CHECK(contains(tma, "g_mutation_audit_wal().enable"), "AC2: WAL enable call");
        // The block must cite #3375 to anchor the regression contract.
        CHECK(contains(tma, "#3375"), "AC2: apply_production_audit_defaults cites #3375");
    }

    // \u2500\u2500 AC3: query:security-posture exposes additive audit-durable-gap \u2500\u2500
    {
        std::println("\n--- AC3: additive audit-durable-gap key ---");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(contains(obs, "audit-durable-gap"),
              "AC3: additive key 'audit-durable-gap' present in obs_eval");
        CHECK(contains(obs, "schema-3375"), "AC3: schema-3375 sentinel present");
        CHECK(contains(obs, "issue-3375"), "AC3: issue-3375 sentinel present");
        // The condition must be: prod && !sandbox_off && strategy==Full && !wal_enabled
        CHECK(contains(obs, "AuditStrategy::Full"),
              "AC3: AuditStrategy::Full check in audit-durable-gap condition");
        CHECK(contains(obs, "g_mutation_audit_wal().is_enabled()"),
              "AC3: wal-enabled check in audit-durable-gap condition");
    }

    // \u2500\u2500 AC4: apply_production_security_defaults force_wal path unchanged \u2500\u2500
    {
        std::println("\n--- AC4: security-defaults force_wal path unchanged ---");
        const auto sec = read_file("src/compiler/security_defaults.hh");
        // The existing security-defaults block must still be there with
        // all the same multi-tenant / Strict / Restricted counters.
        CHECK(contains(sec, "force_wal = multi_tenant || strict || restricted"),
              "AC4: existing force_wal expression in security_defaults");
        CHECK(contains(sec, "audit_wal_forced_by_multi_tenant_total"),
              "AC4: existing multi-tenant force counter");
        CHECK(contains(sec, "audit_wal_forced_by_restricted_total"),
              "AC4: existing restricted-only force counter");
        // #3302 fail-closed pair: still only armed when force_wal actually
        // enables WAL.
        CHECK(contains(sec, "if (force_wal)"), "AC4: force_wal gate present");
    }

    // \u2500\u2500 AC5: no docs/design/3375-*; no test_issue_3375.cpp \u2500\u2500
    {
        std::println("\n--- AC5: no docs/design/3375-*; no test_issue_3375.cpp ---");
        CHECK(read_file("docs/design/3375-audit-durable-gap-force-wal.md").empty(),
              "AC5: no docs/design/3375-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3375.cpp").empty(),
              "AC5: no test_issue_3375.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3375.cpp").empty(),
              "AC5: no tests/issues/test_issue_3375.cpp (R1 abandoned scheme)");
    }

    // ── #3460: force_wal pairs the SE side-car at both defaults sites ──
    {
        std::println("\n--- #3460: SE side-car paired at both force_wal sites ---");
        const auto sec = read_file("src/compiler/security_defaults.hh");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(contains(sec, "Issue #3460"), "3460 AC1: security_defaults cites #3460");
        CHECK(contains(sec, "g_security_event_wal().enable"),
              "3460 AC1: SE pair in security_defaults step 4");
        CHECK(contains(sec, "core/security_event_wal.hh"),
              "3460 AC1: security_defaults includes the side-car header");
        CHECK(contains(tma, "Issue #3460"), "3460 AC2: typed_mutation_audit cites #3460");
        CHECK(contains(tma, "g_security_event_wal().enable"),
              "3460 AC2: SE pair in the #3375 force_wal block");
        const auto esec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(contains(esec, "g_mutation_audit_wal().disable()") &&
                  contains(esec, "g_security_event_wal().disable()"),
              "3460 AC5: disable_mutation_audit_wal still disables both");
        CHECK(read_file("tests/compiler/test_issue_3460.cpp").empty() &&
                  read_file("tests/issues/test_issue_3460.cpp").empty(),
              "3460 AC6: no test_issue_3460.cpp (src-aligned suites only)");
    }

    // ── #3493: Restricted + overflow full → require_effect deny ──
    // Live member (this suite runs in test_security_capability_batch).
    // Overflow deny is the first conjunct in require_effect; Restricted
    // must not keep mutating while the overflow ring is full.
    {
        using aura::compiler::CompilerService;
        using aura::compiler::security::apply_production_security_defaults;
        using aura::compiler::security::kEffectMutate;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        std::println("\n--- #3493: Restricted overflow-full require_effect deny ---");
        const char* prev_sb = std::getenv("AURA_SANDBOX");
        std::string prev_sb_s = prev_sb ? prev_sb : "";
        const char* prev_open = std::getenv("AURA_WAL_APPEND_FAIL_OPEN");
        std::string prev_open_s = prev_open ? prev_open : "";
        ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_audit_defaults();
        apply_production_security_defaults();
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3493 live: fail-closed active under Restricted");
        for (std::uint32_t i = 0; i < aura::core::security_event_wal::kWalOverflowRingCapacity;
             ++i) {
            aura::core::security_event_wal::WalOverflowRecord rec{};
            rec.mid = i + 1;
            rec.reason = "test:3493-fill";
            aura::core::security_event_wal::wal_overflow_ring_push(rec);
        }
        CHECK(aura::core::security_event_wal::wal_overflow_ring_full(),
              "3493 live: overflow ring full");
        CHECK(!ev.require_effect(kEffectMutate, "test:3493-full", 0),
              "3493 live: Restricted + overflow full → deny");
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        apply_dev_audit_defaults();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        if (!prev_sb_s.empty())
            ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
        else
            ::unsetenv("AURA_SANDBOX");
        if (!prev_open_s.empty())
            ::setenv("AURA_WAL_APPEND_FAIL_OPEN", prev_open_s.c_str(), 1);
        else
            ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_durable_gap_force_wal();
}
#endif

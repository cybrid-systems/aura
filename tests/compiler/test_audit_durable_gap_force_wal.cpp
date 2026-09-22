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
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/wal_append_fail_slo.h"
#include "core/workspace_isolation.hh"
#include "orch/security_schedule_gate.h"

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
        // #3881/#3906 moved the posture obs implementation (and the
        // audit-durable-gap key with it) from evaluator_primitives_obs_eval.cpp
        // into evaluator_primitives_security.cpp — cite the new home.
        const auto obs = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(contains(obs, "audit-durable-gap"),
              "AC3: additive key 'audit-durable-gap' present in security obs");
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
            (void)aura::core::security_event_wal::wal_overflow_ring_push(rec);
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

    // ── #3965: SE enable-fail + mutation WAL enable-fail stay fail-closed ──
    {
        using aura::compiler::CompilerService;
        using aura::compiler::security::apply_production_security_defaults;
        using aura::compiler::security::kEffectMutate;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::audit_wal::g_mutation_audit_wal;
        using aura::core::security_event::kSecurityEventRingSize;
        using aura::core::security_event_wal::g_security_event_wal;
        using aura::core::security_event_wal::kWalOverflowRingCapacity;
        using aura::core::security_event_wal::wal_overflow_ring_clear_for_test;
        using aura::core::security_event_wal::wal_overflow_ring_full;
        using aura::core::security_event_wal::wal_overflow_ring_wrap_refuse_total;
        using aura::core::workspace_isolation::g_workspace_isolation;
        std::println("\n--- #3965: SE enable-fail is fail-closed under force_wal ---");
        const auto sec = read_file("src/compiler/security_defaults.hh");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        const auto sew = read_file("src/core/security_event_wal.hh");
        const auto slo = read_file("src/core/wal_append_fail_slo.h");
        CHECK(contains(sec, "Issue #3965"), "3965: security_defaults cites");
        CHECK(contains(tma, "Issue #3965"), "3965: typed_mutation_audit cites");
        CHECK(contains(slo, "kSeWalForceWalEnableFailClosedIssue = 3965"), "3965: issue stamp");
        CHECK(contains(sew, "se-wal-enable-miss"), "3965: WAL-off deny overflow");
        CHECK(contains(sec, "se_ok ="), "3965: SE enable result kept");
        CHECK(!contains(sec, "} else if (force_wal) {"),
              "3965: mutation WAL enable-fail does not disarm fail-closed");
        CHECK(!contains(tma, "} else if (force_wal) {"),
              "3965: audit-defaults mutation miss does not disarm");
        CHECK(sew.find("schema-3965") == std::string::npos, "3965: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3965.cpp").empty(), "3965: no invent");
        CHECK(read_file("docs/design/3965-se-wal-enable-fail.md").empty(), "3965: no docs/design");

        const char* prev_sb = std::getenv("AURA_SANDBOX");
        std::string prev_sb_s = prev_sb ? prev_sb : "";
        const char* prev_open = std::getenv("AURA_WAL_APPEND_FAIL_OPEN");
        std::string prev_open_s = prev_open ? prev_open : "";
        ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_audit_defaults();
        apply_production_security_defaults();
        wal_overflow_ring_clear_for_test();
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3965: fail-closed armed under Restricted force_wal");
        g_mutation_audit_wal().disable();
        CHECK(!g_mutation_audit_wal().enable("", nullptr, 0), "3965: mutation WAL enable-fail");
        CHECK(aura::core::wal_slo::wal_fail_closed_defaulted_by_force_wal() != 0,
              "3965: mutation WAL enable-fail does not clear fail-closed");
        g_security_event_wal().disable();
        CHECK(!g_security_event_wal().is_enabled(),
              "3965: SE sidecar off after enable-fail inject");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3965: SE enable-fail keeps fail-closed");

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_tenant_principal(10, "t10", /*allow_cross=*/false);
        g_workspace_isolation().set_strict_sandbox_linked(true);
        const auto refuse0 = wal_overflow_ring_wrap_refuse_total().load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < kSecurityEventRingSize + 8; ++i) {
            (void)ev.check_workspace_isolation(10, 99, kEffectMutate, "test:3965-iso-storm");
        }
        CHECK(wal_overflow_ring_full() ||
                  wal_overflow_ring_wrap_refuse_total().load(std::memory_order_relaxed) > refuse0,
              "3965: IsolationDeny storm fills overflow ring");
        CHECK(!ev.require_effect(kEffectMutate, "test:3965-no-mutate", 0),
              "3965: overflow/fail-closed denies mutate");

        wal_overflow_ring_clear_for_test();
        apply_dev_audit_defaults();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        ::setenv("AURA_SANDBOX", "off", 1);
        apply_dev_audit_defaults();
        wal_overflow_ring_clear_for_test();
        CompilerService cs_soft;
        auto& ev_soft = cs_soft.evaluator();
        ev_soft.set_effect_sandbox_mode(0);
        g_security_event_wal().disable();
        (void)ev_soft.check_workspace_isolation(10, 99, kEffectMutate, "test:3965-soft");
        CHECK(!wal_overflow_ring_full(), "3965: Soft IsolationDeny does not arm overflow");
        CHECK(!aura::core::wal_slo::wal_append_fail_closed_active(),
              "3965: Soft fail-closed stays off");

        wal_overflow_ring_clear_for_test();
        if (!prev_sb_s.empty())
            ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
        else
            ::unsetenv("AURA_SANDBOX");
        if (!prev_open_s.empty())
            ::setenv("AURA_WAL_APPEND_FAIL_OPEN", prev_open_s.c_str(), 1);
        else
            ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
    }

    // ── #4005: force_wal pair enable-fail is a posture breach ──
    {
        using aura::compiler::CompilerService;
        using aura::compiler::security::kEffectMutate;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::audit_wal::g_audit_wal_metrics;
        using aura::core::audit_wal::g_mutation_audit_wal;
        using aura::core::audit_wal::reset_audit_wal_for_test;
        using aura::core::security_event::g_security_event_ring;
        using aura::core::security_event::kSecurityEventRingSize;
        using aura::core::security_event::SecurityEventKind;
        using aura::core::security_event_wal::g_security_event_wal;
        using aura::core::security_event_wal::kWalOverflowRingCapacity;
        using aura::core::security_event_wal::wal_overflow_ring_clear_for_test;
        using aura::core::security_event_wal::wal_overflow_ring_push;
        using aura::core::wal_slo::decide_wal_append_fail_slo;
        using aura::core::wal_slo::kWalForceWalEnableFailIssue;
        using aura::core::wal_slo::WalAppendFailSloInput;
        using aura::orch::admit_security_schedule;
        using aura::orch::decide_security_schedule;
        using aura::orch::make_security_schedule_input_live;
        using aura::orch::reset_orch_security_schedule_counters_for_test;
        using aura::orch::SecurityScheduleForceReason;
        using aura::orch::wal_enable_failed_would_arm_live;
        std::println("\n--- #4005: force_wal enable-fail is a posture breach ---");

        const auto sec = read_file("src/compiler/security_defaults.hh");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        const auto slo = read_file("src/core/wal_append_fail_slo.h");
        const auto gate = read_file("src/orch/security_schedule_gate.h");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto sew = read_file("src/core/security_event_wal.hh");
        CHECK(contains(slo, "kWalForceWalEnableFailIssue = 4005"), "4005: issue stamp");
        CHECK(contains(slo, "force_wal_enable_failed"), "4005: decide input field");
        CHECK(contains(slo, "note_wal_enable_failed"), "4005: note_wal_enable_failed");
        CHECK(contains(slo, "wal-enable-failed"), "4005: distinct posture key");
        CHECK(contains(tma, "wal_ready = mut_ok && se_ok"), "4005: audit-defaults consumes se_ok");
        CHECK(contains(sec, "wal_ready = mut_ok && se_ok"),
              "4005: security-defaults consumes se_ok");
        CHECK(!contains(tma, "(void)se_ok"), "4005: (void)se_ok gone from audit-defaults");
        CHECK(!contains(sec, "(void)se_ok"), "4005: (void)se_ok gone from security-defaults");
        CHECK(contains(gate, "wal_enable_failed"), "4005: schedule input field");
        CHECK(contains(gate, "wal-enable-failed"), "4005: schedule force_reason string");
        CHECK(contains(prim, "schema-4005"), "4005: additive schema-4005");
        CHECK(contains(prim, "force-wal-enable-fail-total"), "4005: additive counter key");
        CHECK(contains(prim, "insert_kv(\"wal-enable-failed\""), "4005: posture key");
        CHECK(contains(sew, "overflow-refuse"), "4005: overflow refuse marker");
        CHECK(contains(prim, "wal-append-fail-breach"),
              "4005: did not rename wal-append-fail-breach");
        CHECK(contains(prim, "audit-durable-gap"), "4005: did not rename audit-durable-gap");
        CHECK(read_file("tests/compiler/test_issue_4005.cpp").empty(),
              "4005: no test_issue_4005.cpp");
        CHECK(read_file("docs/design/4005-wal-enable-fail.md").empty(), "4005: no docs/design");

        {
            WalAppendFailSloInput pin;
            pin.wal_enabled = false;
            pin.production_defaults = true;
            pin.soft_mode = false;
            const auto hist = decide_wal_append_fail_slo(pin);
            CHECK(!hist.breached && hist.force_reason == "ok",
                  "4005: default !wal_enabled stays historical ok (#3056)");
            pin.force_wal_enable_failed = true;
            const auto d1 = decide_wal_append_fail_slo(pin);
            const auto d2 = decide_wal_append_fail_slo(pin);
            CHECK(d1.would_arm_degraded == d2.would_arm_degraded && d1.breached == d2.breached,
                  "4005: decide is pure");
            CHECK(d1.breached && d1.would_arm_degraded, "4005: production enable-fail arms");
            CHECK(d1.force_reason == "wal-enable-failed", "4005: force_reason wal-enable-failed");
            CHECK(d1.force_reason_code == 3, "4005: force_reason_code=3");
            pin.soft_mode = true;
            const auto ds = decide_wal_append_fail_slo(pin);
            CHECK(ds.breached && !ds.would_arm_degraded, "4005: Soft observe-only");
            CHECK(ds.force_reason == "soft-breach-observe", "4005: Soft force_reason");
        }

        {
            auto in = aura::orch::SecurityScheduleInput{};
            in.production_mode = true;
            in.soft_mode = false;
            in.wal_enable_failed = true;
            in.posture_wal_off_restricted = true;
            const auto d = decide_security_schedule(in);
            CHECK(!d.would_allow_new_mutate, "4005: schedule denies");
            CHECK(d.force_reason == SecurityScheduleForceReason::wal_enable_failed,
                  "4005: wal-enable-failed before posture-degraded");
            in.wal_enable_failed = false;
            const auto dp = decide_security_schedule(in);
            CHECK(dp.force_reason == SecurityScheduleForceReason::posture_degraded,
                  "4005: posture-degraded still live without enable-fail flag");
            in.soft_mode = true;
            in.wal_enable_failed = true;
            const auto ds = decide_security_schedule(in);
            CHECK(ds.would_allow_new_mutate, "4005: Soft schedule never denies");
        }

        const char* prev_sb = std::getenv("AURA_SANDBOX");
        std::string prev_sb_s = prev_sb ? prev_sb : "";
        const char* prev_mt = std::getenv("AURA_MULTI_TENANT");
        std::string prev_mt_s = prev_mt ? prev_mt : "";
        const char* prev_wal = std::getenv("AURA_MUTATION_AUDIT_WAL");
        std::string prev_wal_s = prev_wal ? prev_wal : "";
        const char* prev_persist = std::getenv("AURA_PERSIST_DIR");
        std::string prev_persist_s = prev_persist ? prev_persist : "";
        const char* prev_open = std::getenv("AURA_WAL_APPEND_FAIL_OPEN");
        std::string prev_open_s = prev_open ? prev_open : "";

        auto restore_env = [&]() {
            if (!prev_sb_s.empty())
                ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
            else
                ::unsetenv("AURA_SANDBOX");
            if (!prev_mt_s.empty())
                ::setenv("AURA_MULTI_TENANT", prev_mt_s.c_str(), 1);
            else
                ::unsetenv("AURA_MULTI_TENANT");
            if (!prev_wal_s.empty())
                ::setenv("AURA_MUTATION_AUDIT_WAL", prev_wal_s.c_str(), 1);
            else
                ::unsetenv("AURA_MUTATION_AUDIT_WAL");
            if (!prev_persist_s.empty())
                ::setenv("AURA_PERSIST_DIR", prev_persist_s.c_str(), 1);
            else
                ::unsetenv("AURA_PERSIST_DIR");
            if (!prev_open_s.empty())
                ::setenv("AURA_WAL_APPEND_FAIL_OPEN", prev_open_s.c_str(), 1);
            else
                ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
        };

        reset_audit_wal_for_test();
        g_security_event_wal().disable();
        wal_overflow_ring_clear_for_test();
        ::aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
        reset_orch_security_schedule_counters_for_test();
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
        ::unsetenv("AURA_PERSIST_DIR");
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        ::setenv("AURA_MUTATION_AUDIT_WAL", "/proc/nonwritable", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_audit_defaults();
        CHECK(g_audit_wal_metrics().force_wal_enable_fail_total.load(std::memory_order_relaxed) ==
                  1,
              "4005 live: force_wal_enable_fail_total==1 after one apply");
        CHECK(::aura::core::wal_slo::force_wal_enable_failed_observed() != 0,
              "4005 live: observed");
        CHECK(::aura::core::wal_slo::force_wal_enable_failed() != 0, "4005 live: admission armed");
        CHECK(!g_mutation_audit_wal().is_enabled(), "4005 live: mutation WAL off");
        CHECK(wal_enable_failed_would_arm_live(/*prod=*/true, /*soft=*/false),
              "4005 live: schedule live helper arms");
        CHECK(!wal_enable_failed_would_arm_live(/*prod=*/true, /*soft=*/true),
              "4005 live: Soft live helper stays quiet");
        reset_orch_security_schedule_counters_for_test();
        const auto live_in =
            make_security_schedule_input_live(/*Restricted=*/1, /*prod=*/true, /*soft=*/false);
        CHECK(live_in.wal_enable_failed, "4005 live: live input wal_enable_failed");
        const auto rej = admit_security_schedule(live_in);
        CHECK(rej.has_value(), "4005 live: next mutate schedule-denied");
        CHECK(rej.value_or("").find("wal-enable-failed") != std::string::npos,
              "4005 live: deny reason wal-enable-failed");
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        CHECK(!ev.require_effect(kEffectMutate, "test:4005-no-mutate", 0),
              "4005 live: outermost mutate denied");

        wal_overflow_ring_clear_for_test();
        for (std::uint32_t i = 0; i < kWalOverflowRingCapacity; ++i) {
            aura::core::security_event_wal::WalOverflowRecord rec{};
            rec.mid = 4005000u + i;
            rec.reason = "test:4005-fill";
            CHECK(wal_overflow_ring_push(rec), "4005: fill overflow ok");
        }
        aura::core::security_event_wal::WalOverflowRecord ovr{};
        ovr.mid = 4005999;
        ovr.tenant_id = 7;
        ovr.reason = "test:4005-refuse";
        ovr.op = "require_effect";
        CHECK(!wal_overflow_ring_push(ovr), "4005: overflow refuse under fail-closed");
        {
            auto& ring = g_security_event_ring();
            const auto seq = ring.seq.load(std::memory_order_relaxed);
            CHECK(seq > 0, "4005: SE ring advanced on refuse marker");
            const auto& slot = ring.ring[(seq - 1) % kSecurityEventRingSize];
            CHECK(slot.kind == SecurityEventKind::PostureObserve,
                  "4005: marker kind PostureObserve");
            CHECK(std::string_view(slot.reason) == "overflow-refuse",
                  "4005: marker reason overflow-refuse");
            CHECK(slot.mutation_id == 4005999, "4005: marker preserves mid");
        }

        reset_audit_wal_for_test();
        g_security_event_wal().disable();
        ::aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
        ::setenv("AURA_WAL_APPEND_FAIL_OPEN", "1", 1);
        ::unsetenv("AURA_SANDBOX");
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        ::setenv("AURA_MUTATION_AUDIT_WAL", "/proc/nonwritable", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_audit_defaults();
        CHECK(g_audit_wal_metrics().force_wal_enable_fail_total.load(std::memory_order_relaxed) >=
                  1,
              "4005 FAIL_OPEN: counter still bumps");
        CHECK(::aura::core::wal_slo::force_wal_enable_failed_observed() != 0,
              "4005 FAIL_OPEN: observed");
        CHECK(::aura::core::wal_slo::force_wal_enable_failed() == 0,
              "4005 FAIL_OPEN: admission not armed");
        CHECK(!wal_enable_failed_would_arm_live(/*prod=*/true, /*soft=*/false),
              "4005 FAIL_OPEN: live helper observe-only");

        wal_overflow_ring_clear_for_test();
        apply_dev_audit_defaults();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        restore_env();
        CHECK(!::aura::core::wal_slo::force_wal_enable_failed(),
              "4005: Soft/dev disarms enable-fail");
        CHECK(!wal_enable_failed_would_arm_live(/*prod=*/false, /*soft=*/true),
              "4005: Soft live helper false");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_durable_gap_force_wal();
}
#endif

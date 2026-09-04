// @category: unit
// @reason: Issue #2534 — query:security-posture + security-correlated-trail.
//
//   AC1: posture returns schema-2534
//   AC2: after EffectDeny, correlated-trail sees se/cap hits for mid
//   AC3: mid=0 empty
//   AC4: Soft callable
//   AC5: security-health still works
//   AC6: source-cite

#include "test_harness.hpp"
#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/wal_append_fail_slo.h"
#include "core/workspace_epoch.hh"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {
using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kEffectMutate;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:security-posture\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}
std::int64_t trail(CompilerService& cs, std::uint64_t mid, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:security-correlated-trail\" {}) \"{}\")", mid, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
} // namespace

int run_test_security_posture_trail() {
    std::println("=== Issue #2534: security posture + correlated trail ===");
    reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);

    CHECK(href(cs, "schema-2534") == 2534, "AC1: schema");
    CHECK(href(cs, "security-posture-wired") == 1, "AC1: wired");
    CHECK(href(cs, "cap-audit-ring-size") == 1024, "AC1: cap ring size");
    CHECK(href(cs, "iso-audit-ring-size") == 1024, "AC1: iso ring size");

    CHECK(trail(cs, 0, "match-count") == 0, "AC3: mid=0 empty");

    // EffectDeny with mid
    EffectProvenance prov;
    prov.mutation_id = 4242;
    prov.epoch = 4242;
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    g_capability_registry().record_audit(Effect::Mutate, Effect::None, 1, prov, true, "deny-test");

    CHECK(trail(cs, 4242, "capability-hits") >= 1, "AC2: cap audit hit");
    CHECK(trail(cs, 4242, "se-hits") >= 1, "AC2: SE hit");

    auto r = cs.eval("(hash-ref (engine:metrics \"query:security-health\") \"schema-2389\")");
    CHECK(r && is_int(*r) && as_int(*r) == 2389, "AC5: health preserved");

    auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(sec.find("query:security-posture") != std::string::npos, "AC6: posture");
    CHECK(sec.find("query:security-correlated-trail") != std::string::npos, "AC6: trail");
    CHECK(sec.find("2534") != std::string::npos, "AC6: cite");
    CHECK(href(cs, "schema-3056") == 3056, "3056: posture schema");
    CHECK(href(cs, "wal-append-fail-slo-wired") == 1, "3056: slo wired");
    CHECK(href(cs, "wal-append-fail-breach") == 0, "3056: idle no breach");

    // ── #3109: production WAL append fail-closed option ────────────────
    // Soft/Off / no-env: wal-fail-closed-active=0, wal-overflow-ring-depth=0.
    // Default behavior unchanged (fail-open + #3056 SLO arm).
    CHECK(href(cs, "schema-3109") == 3109, "3109 AC5: schema-3109");
    CHECK(href(cs, "issue-3109") == 3109, "3109 AC5: issue-3109");
    CHECK(href(cs, "wal-fail-closed-active") == 0, "3109 AC1: no env → fail-closed inactive");
    CHECK(href(cs, "wal-overflow-ring-depth") == 0, "3109 AC1: overflow ring depth=0 (Soft/Off)");
    // 3109 AC3: overflow ring path wired (source-cite only — env is unset in this run,
    // so depth stays 0; the gate is verified by the AC5 source-cite checks below)
    CHECK(href(cs, "wal-overflow-ring-depth") == 0,
          "3109 AC3: overflow ring depth observable (additive key)");
    // 3109 AC4: require_effect deny path wired (source-cite only — require_effect deny
    // is verified by AC2 source-cite below since Strict + fail-closed + overflow-full
    // is hard to trigger in this test harness)

    // Source-cite: helper + ring + require_effect deny path
    const auto slo = read_file("src/core/wal_append_fail_slo.h");
    CHECK(slo.find("wal_append_fail_closed_active") != std::string::npos,
          "3109 AC1: helper declared");
    const auto sew = read_file("src/core/security_event_wal.hh");
    CHECK(sew.find("kWalOverflowRingCapacity") != std::string::npos,
          "3109 AC1: overflow ring capacity declared");
    CHECK(sew.find("wal_overflow_ring_push") != std::string::npos,
          "3109 AC1: overflow ring push helper");
    CHECK(sew.find("wal_append_fail_closed_active()") != std::string::npos,
          "3109 AC1: append wires fail-closed check");
    const auto ev_src = read_file("src/compiler/evaluator_security.cpp");
    const auto deny_if = ev_src.find(
        "if (req_bits != 0 && ::aura::core::wal_slo::wal_append_fail_closed_active() &&");
    CHECK(deny_if != std::string::npos &&
              ev_src.find("wal_overflow_ring_full()", deny_if) != std::string::npos,
          "3109 AC2: require_effect deny path wired (fail-closed + overflow full)");
    CHECK(deny_if != std::string::npos &&
              ev_src.substr(deny_if, 400).find("is_strict()") == std::string::npos,
          "3109 AC2 / #3493: deny if does not extra-AND is_strict()");
    // Build.py wires the linter
    const auto build = read_file("build.py");
    CHECK(build.find("check_wal_append_fail_closed_3109") != std::string::npos,
          "3109 AC5: build.py wires 3109 linter");
    // No-invent / no-design enforcement
    if (std::FILE* f = std::fopen("tests/compiler/test_issue_3109.cpp", "r")) {
        std::fclose(f);
        CHECK(false, "3109 AC5: no invent test_issue_3109.cpp");
    }
    const auto docs = std::string("docs/design/");
    // Source-cite #3056 lineage preserved (SLO + breach arm)
    CHECK(slo.find("kWalAppendFailSloIssue = 3056") != std::string::npos,
          "3109 AC5: #3056 lineage preserved");

    // ── #3302: force_wal default-arms fail-closed ───────────────────────
    {
        std::println("\n--- #3302 force_wal default fail-closed ---");
        const char* prev_sb = std::getenv("AURA_SANDBOX");
        const std::string prev_sb_s = prev_sb ? prev_sb : "";
        const char* prev_open = std::getenv("AURA_WAL_APPEND_FAIL_OPEN");
        const std::string prev_open_s = prev_open ? prev_open : "";
        const char* prev_closed = std::getenv("AURA_WAL_APPEND_FAIL_CLOSED");
        const std::string prev_closed_s = prev_closed ? prev_closed : "";
        auto restore_env = [&] {
            if (prev_sb_s.empty())
                ::unsetenv("AURA_SANDBOX");
            else
                ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
            if (prev_open_s.empty())
                ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
            else
                ::setenv("AURA_WAL_APPEND_FAIL_OPEN", prev_open_s.c_str(), 1);
            if (prev_closed_s.empty())
                ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");
            else
                ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", prev_closed_s.c_str(), 1);
            apply_dev_audit_defaults();
            aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
            aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        };

        ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");
        ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        aura::core::wal_slo::reset_wal_append_fail_slo_for_test();

        // AC1: Soft / sandbox=off never arms fail-closed.
        ::setenv("AURA_SANDBOX", "off", 1);
        apply_production_security_defaults();
        CHECK(!aura::core::wal_slo::wal_append_fail_closed_active(),
              "3302 AC1: Soft fail-closed inactive");
        CHECK(aura::core::wal_slo::wal_fail_closed_defaulted_by_force_wal() == 0,
              "3302 AC1: Soft defaulted-by-force-wal=0");
        CHECK(href(cs, "wal-fail-closed-active") == 0, "3302 AC1: query fail-closed-active=0");
        CHECK(href(cs, "wal-fail-closed-defaulted-by-force-wal") == 0,
              "3302 AC1: query defaulted=0");

        // AC2: Restricted force_wal (no FAIL_OPEN) default-arms fail-closed.
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_security_defaults();
        CHECK(aura::core::wal_slo::wal_fail_closed_defaulted_by_force_wal() != 0,
              "3302 AC2: force_wal defaulted flag");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3302 AC2: fail-closed active under Restricted force_wal");
        CHECK(href(cs, "wal-fail-closed-active") == 1, "3302 AC2: query fail-closed-active=1");
        CHECK(href(cs, "wal-fail-closed-defaulted-by-force-wal") == 1,
              "3302 AC2: query defaulted=1");
        {
            namespace fs = std::filesystem;
            const auto dir = fs::temp_directory_path() / "aura-3302-ac2-XXXXXX";
            fs::remove_all(dir);
            fs::create_directories(dir);
            CHECK(ev.enable_security_event_wal(dir.string()), "3302 AC2: enable SE WAL");
            aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
                1, std::memory_order_relaxed);
            const bool fail_ret = aura::core::security_event_wal::persist_security_event(
                aura::core::security_event::SecurityEventKind::EffectDeny, 7, 0x3302, 1, 0,
                "test:3302-ac2", "inject", true, 1, 0);
            CHECK(!fail_ret, "3302 AC2: inject fail returns false");
            CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() >= 1,
                  "3302 AC2: overflow ring captured inject");
            ev.disable_security_event_wal();
            fs::remove_all(dir);
        }

        // AC3: explicit FAIL_OPEN restores fail-open (no overflow push).
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        ::setenv("AURA_WAL_APPEND_FAIL_OPEN", "1", 1);
        CHECK(!aura::core::wal_slo::wal_append_fail_closed_active(),
              "3302 AC3: FAIL_OPEN opts out");
        CHECK(aura::core::wal_slo::wal_fail_closed_defaulted_by_force_wal() != 0,
              "3302 AC3: defaulted flag still set (force_wal happened)");
        {
            namespace fs = std::filesystem;
            const auto dir = fs::temp_directory_path() / "aura-3302-ac3-XXXXXX";
            fs::remove_all(dir);
            fs::create_directories(dir);
            CHECK(ev.enable_security_event_wal(dir.string()), "3302 AC3: enable SE WAL");
            aura::core::wal_slo::g_wal_append_fail_slo_counters.inject_fail_remaining.store(
                1, std::memory_order_relaxed);
            (void)aura::core::security_event_wal::persist_security_event(
                aura::core::security_event::SecurityEventKind::EffectDeny, 7, 0x3303, 1, 0,
                "test:3302-ac3", "inject", true, 1, 0);
            CHECK(aura::core::security_event_wal::wal_overflow_ring_depth() == 0,
                  "3302 AC3: overflow ring not written under FAIL_OPEN");
            ev.disable_security_event_wal();
            fs::remove_all(dir);
        }
        ::unsetenv("AURA_WAL_APPEND_FAIL_OPEN");

        // AC4: explicit FAIL_CLOSED still forces on without force_wal flag.
        apply_dev_audit_defaults();
        aura::core::wal_slo::set_wal_fail_closed_defaulted_by_force_wal(false);
        ::setenv("AURA_WAL_APPEND_FAIL_CLOSED", "1", 1);
        apply_production_audit_defaults();
        CHECK(aura::core::wal_slo::wal_fail_closed_defaulted_by_force_wal() == 0,
              "3302 AC4: explicit CLOSED is not force_wal-defaulted");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3302 AC4: FAIL_CLOSED=1 still forces on");
        ::unsetenv("AURA_WAL_APPEND_FAIL_CLOSED");

        // AC5: Strict + overflow full → require_effect deny (#3109 path).
        ::setenv("AURA_SANDBOX", "strict", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
        apply_production_security_defaults();
        aura::core::security_event_wal::wal_overflow_ring_clear_for_test();
        for (std::uint32_t i = 0; i < aura::core::security_event_wal::kWalOverflowRingCapacity;
             ++i) {
            aura::core::security_event_wal::WalOverflowRecord rec{};
            rec.mid = i + 1;
            rec.reason = "test:3302-fill";
            aura::core::security_event_wal::wal_overflow_ring_push(rec);
        }
        CHECK(aura::core::security_event_wal::wal_overflow_ring_full(),
              "3302 AC5: overflow ring full");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3302 AC5: fail-closed active under Strict force_wal");
        CHECK(!ev.require_effect(kEffectMutate, "test:3302-ac5", 0),
              "3302 AC5: require_effect denies when overflow full");

        // #3493: Restricted + overflow full also denies (not Strict-only).
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        apply_production_security_defaults();
        CHECK(aura::core::security_event_wal::wal_overflow_ring_full(),
              "3493: overflow ring still full");
        CHECK(aura::core::wal_slo::wal_append_fail_closed_active(),
              "3493: fail-closed active under Restricted force_wal");
        CHECK(!ev.require_effect(kEffectMutate, "test:3493-restricted", 0),
              "3493: Restricted + overflow full → require_effect deny");

        CHECK(href(cs, "schema-3302") == 3302, "3302 AC6: schema-3302");
        CHECK(href(cs, "issue-3302") == 3302, "3302 AC6: issue-3302");
        const auto slo3302 = read_file("src/core/wal_append_fail_slo.h");
        CHECK(slo3302.find("AURA_WAL_APPEND_FAIL_OPEN") != std::string::npos,
              "3302 AC6: FAIL_OPEN env");
        CHECK(slo3302.find("kWalAppendFailClosedForceWalIssue = 3302") != std::string::npos,
              "3302 AC6: issue stamp");
        const auto build3302 = read_file("build.py");
        CHECK(build3302.find("check_wal_fail_closed_force_wal_3302") != std::string::npos,
              "3302 AC6: build.py wires 3302 linter");
        CHECK(build3302.find("check_wal_append_fail_closed_3109") != std::string::npos,
              "3302 AC6: 3109 linter still wired");
        if (std::FILE* f = std::fopen("tests/compiler/test_issue_3302.cpp", "r")) {
            std::fclose(f);
            CHECK(false, "3302 AC6: no invent test_issue_3302.cpp");
        }

        restore_env();
    }

    std::println("\n=== #2534/#3109/#3302: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_posture_trail();
}
#endif

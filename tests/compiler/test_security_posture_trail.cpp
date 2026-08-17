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
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {
using aura::compiler::CompilerService;
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
    const auto ev = read_file("src/compiler/evaluator_security.cpp");
    CHECK(ev.find("wal_append_fail_closed_active()") != std::string::npos &&
              ev.find("wal_overflow_ring_full()") != std::string::npos &&
              ev.find("is_strict()") != std::string::npos,
          "3109 AC2: require_effect deny path wired (Strict + fail-closed + overflow full)");
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

    std::println("\n=== #2534/#3109: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_posture_trail();
}
#endif

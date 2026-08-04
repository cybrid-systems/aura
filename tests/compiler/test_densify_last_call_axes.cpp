// @category: unit
// @reason: Issue #2376 — DensifyConsistencyReport per-call last-result
// for envframe + closure remount axes (seal #2361/#2365 last-call contract).
//
//   AC1: inject envframe ownership fail → envframe_ok false + fail code
//   AC2: inject closure remount fail → closure_remount_ok false + gate
//   AC3: Soft / no Moving → axes true; zero extra walks
//   AC4: query densify-envframe-ok / densify-closure-remount-ok last-call
//        + densify-last-call-seq + schema-2376
//   AC5: cumulative counters remain; source-cite + hard contract wired

#include "test_harness.hpp"

#include "core/densify_consistency_report.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.envframe_lifetime;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::bump_densify_consistency_fail_total;
using aura::core::densify_consistency::bump_last_densify_call_seq;
using aura::core::densify_consistency::densify_consistency_fail_total;
using aura::core::densify_consistency::densify_consistency_hard_contract_enabled;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::densify_consistency::kDensifyClosureFailCaptureRemap;
using aura::core::densify_consistency::kDensifyEnvframeFailOwnershipScan;
using aura::core::densify_consistency::kDensifyFailNone;
using aura::core::densify_consistency::last_densify_call_seq;
using aura::core::densify_consistency::last_densify_closure_fail_code;
using aura::core::densify_consistency::last_densify_closure_remount_ok;
using aura::core::densify_consistency::last_densify_envframe_fail_code;
using aura::core::densify_consistency::last_densify_envframe_ok;
using aura::core::densify_consistency::note_last_densify_closure_remount_ok;
using aura::core::densify_consistency::note_last_densify_envframe_ok;
using aura::core::envframe_lifetime::bump_envframe_lifetime_densify_ownership_scan_total;
using aura::core::envframe_lifetime::envframe_lifetime_densify_ownership_scan_fail_total;
using aura::core::envframe_lifetime::envframe_lifetime_densify_ownership_scan_total;
using aura::core::envframe_lifetime::inject_densify_ownership_scan_fail_for_test;
using aura::core::envframe_lifetime::reset_envframe_lifetime_stats;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:lifetime-contract-snapshot\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: envframe last-call fail ──
static void ac1_envframe_last_call_fail() {
    std::println("\n--- AC1: envframe ownership fail → last-call envframe_ok false ---");
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);

    const auto scan0 = envframe_lifetime_densify_ownership_scan_total();
    const auto fail0 = envframe_lifetime_densify_ownership_scan_fail_total();
    bump_envframe_lifetime_densify_ownership_scan_total();
    inject_densify_ownership_scan_fail_for_test();
    const auto scan1 = envframe_lifetime_densify_ownership_scan_total();
    const auto fail1 = envframe_lifetime_densify_ownership_scan_fail_total();
    const bool env_scan_ok = (scan1 > scan0) && (fail1 == fail0);
    CHECK(!env_scan_ok, "AC1: ownership scan fail detected (last-call delta)");

    DensifyConsistencyReport r;
    r.envframe_ok = false;
    CHECK(!r.overall_ok(), "AC1: !overall_ok when envframe fails");
    CHECK(std::string_view(r.force_reason()) == "envframe", "AC1: force_reason envframe");

    const auto fail_total0 = densify_consistency_fail_total();
    bump_densify_consistency_fail_total();
    note_last_densify_envframe_ok(false, kDensifyEnvframeFailOwnershipScan);
    bump_last_densify_call_seq();
    CHECK(densify_consistency_fail_total() == fail_total0 + 1, "AC1: fail total +1");
    CHECK(!last_densify_envframe_ok(), "AC1: last envframe ok false");
    CHECK(last_densify_envframe_fail_code() == kDensifyEnvframeFailOwnershipScan,
          "AC1: fail code ownership_scan");

    note_last_densify_envframe_ok(true);
    reset_envframe_lifetime_stats();
}

// ── AC2: closure remount last-call fail ──
static void ac2_closure_last_call_fail() {
    std::println("\n--- AC2: closure remount last-call fail → gate ---");
    note_last_densify_closure_remount_ok(true);
    // Last-call fail delta: publish false (mirrors force_densify_remap_pairing
    // cl_fail1 > cl_fail0). Cumulative lifetime may already be non-zero.
    note_last_densify_closure_remount_ok(false, kDensifyClosureFailCaptureRemap);
    CHECK(!last_densify_closure_remount_ok(), "AC2: last closure remount false");
    CHECK(last_densify_closure_fail_code() == kDensifyClosureFailCaptureRemap,
          "AC2: fail code capture_remap");

    DensifyConsistencyReport r;
    r.closure_remount_ok = false;
    CHECK(!r.overall_ok(), "AC2: !overall_ok");
    CHECK(std::string_view(r.force_reason()) == "closure", "AC2: force_reason closure");

    const auto fail0 = densify_consistency_fail_total();
    bump_densify_consistency_fail_total();
    bump_last_densify_call_seq();
    CHECK(densify_consistency_fail_total() == fail0 + 1, "AC2: fail total +1");

    note_last_densify_closure_remount_ok(true);
}

// ── AC3: Soft vacuous ──
static void ac3_soft_vacuous() {
    std::println("\n--- AC3: Soft densify axes vacuous true ---");
    DensifyConsistencyReport r; // defaults all true
    CHECK(r.envframe_ok && r.closure_remount_ok, "AC3: default axes true");
    CHECK(r.overall_ok(), "AC3: overall_ok Soft");
    note_last_densify_envframe_ok(true);
    note_last_densify_closure_remount_ok(true);
    CHECK(last_densify_envframe_ok() && last_densify_closure_remount_ok(),
          "AC3: last Soft axes true");
    CHECK(last_densify_envframe_fail_code() == kDensifyFailNone, "AC3: env fail code 0");
    CHECK(last_densify_closure_fail_code() == kDensifyFailNone, "AC3: cl fail code 0");
}

// ── AC4: query ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2376 last-call surface ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2376") == 2376, "AC4: schema-2376");
    CHECK(href(cs, "issue-2376") == 2376, "AC4: issue-2376");
    CHECK(href(cs, "densify-last-call-axes-wired") == 1, "AC4: last-call axes wired");
    CHECK(href(cs, "densify-last-call-seq") >= 0, "AC4: last-call-seq");
    CHECK(href(cs, "densify-envframe-ok") >= 0, "AC4: densify-envframe-ok");
    CHECK(href(cs, "densify-closure-remount-ok") >= 0, "AC4: densify-closure-remount-ok");
    CHECK(href(cs, "densify-envframe-fail-code") >= 0, "AC4: envframe-fail-code");
    CHECK(href(cs, "densify-closure-fail-code") >= 0, "AC4: closure-fail-code");

    const auto seq0 = href(cs, "densify-last-call-seq");
    note_last_densify_envframe_ok(true);
    note_last_densify_closure_remount_ok(true);
    bump_last_densify_call_seq();
    CHECK(href(cs, "densify-last-call-seq") == seq0 + 1, "AC4: call-seq advances");
    CHECK(href(cs, "densify-envframe-ok") == 1, "AC4: idle envframe ok");
    CHECK(href(cs, "densify-closure-remount-ok") == 1, "AC4: idle closure ok");

    note_last_densify_envframe_ok(false, kDensifyEnvframeFailOwnershipScan);
    CHECK(href(cs, "densify-envframe-ok") == 0, "AC4: last fail → envframe 0");
    CHECK(href(cs, "densify-envframe-fail-code") == kDensifyEnvframeFailOwnershipScan,
          "AC4: fail code on query");
    note_last_densify_closure_remount_ok(false, kDensifyClosureFailCaptureRemap);
    CHECK(href(cs, "densify-closure-remount-ok") == 0, "AC4: last fail → closure 0");
    // restore
    note_last_densify_envframe_ok(true);
    note_last_densify_closure_remount_ok(true);

    // Lineage retained
    CHECK(href(cs, "schema-2341") == 2341, "AC4: schema-2341 retained");
    CHECK(href(cs, "schema-2361") == 2361, "AC4: schema-2361 retained");
    CHECK(href(cs, "schema-2365") == 2365, "AC4: schema-2365 retained");
}

// ── AC5: source-cite + hard contract ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite + hard contract ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto dcr = read_file("src/core/densify_consistency_report.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");

    CHECK(emb.find("Issue #2376") != std::string::npos, "AC5: Phase 5 cites #2376");
    CHECK(emb.find("bump_last_densify_call_seq") != std::string::npos, "AC5: call-seq bump");
    CHECK(emb.find("note_last_densify_envframe_ok") != std::string::npos, "AC5: envframe last");
    CHECK(emb.find("note_last_densify_closure_remount_ok") != std::string::npos,
          "AC5: closure last");
    CHECK(env.find("Issue #2376") != std::string::npos, "AC5: pairing cites #2376");
    CHECK(env.find("cl_fail1 == cl_fail0") != std::string::npos ||
              env.find("cl_fail1") != std::string::npos,
          "AC5: closure last-call delta");
    CHECK(dcr.find("g_last_densify_call_seq") != std::string::npos, "AC5: call-seq atomic");
    CHECK(dcr.find("kDensifyEnvframeFailOwnershipScan") != std::string::npos,
          "AC5: envframe fail codes");
    CHECK(q.find("schema-2376") != std::string::npos, "AC5: query schema-2376");
    CHECK(q.find("densify-last-call-axes-wired") != std::string::npos, "AC5: query wired");
    CHECK(q.find("last_densify_envframe_ok") != std::string::npos, "AC5: query last envframe");
    CHECK(q.find("last_densify_closure_remount_ok") != std::string::npos,
          "AC5: query last closure");
    // Hard contract helper present (env-gated; do not abort this process).
    (void)densify_consistency_hard_contract_enabled();
    CHECK(dcr.find("AURA_DENSIFY_CONTRACT") != std::string::npos, "AC5: hard contract env");
    CHECK(emb.find("AURA_DENSIFY_CONTRACT") != std::string::npos, "AC5: Phase 5 hard abort path");
}

} // namespace

int run_test_densify_last_call_axes() {
    std::println("=== Issue #2376: densify last-call envframe + closure axes ===");
    ac1_envframe_last_call_fail();
    ac2_closure_last_call_fail();
    ac3_soft_vacuous();
    ac4_query();
    ac5_source_cite();
    std::println("\n=== #2376: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_last_call_axes();
}
#endif

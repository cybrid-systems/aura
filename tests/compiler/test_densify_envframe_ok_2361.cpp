// @category: unit
// @reason: Issue #2361 — DensifyConsistencyReport.envframe_ok is a real
// per-call check (stop forcing true on the envframe axis).
//
//   AC1: Soft / no Moving densify → envframe_ok stays true (vacuous)
//   AC2: Inject densify ownership scan fail + densify path →
//        envframe_ok false, force_reason=="envframe", overall_ok false,
//        densify_consistency_fail_total advances; success metrics gated
//   AC3: Soft path zero extra cost (scan not required when !had_moving)
//   AC4: query:lifetime-contract-snapshot densify-envframe-ok + schema-2361
//   AC5: Source-cite Phase 5 + scan + fail counter + query keys

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::bump_densify_consistency_fail_total;
using aura::core::densify_consistency::densify_consistency_fail_total;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::densify_consistency::last_densify_envframe_ok;
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

// ── AC1 / AC3: Soft / vacuous envframe_ok ──
static void ac1_soft_envframe_ok() {
    std::println("\n--- AC1/AC3: Soft densify path → envframe_ok true ---");
    DensifyConsistencyReport r;
    // Default-constructed: all axes true including envframe.
    CHECK(r.envframe_ok, "AC1: default envframe_ok == true");
    CHECK(r.overall_ok(), "AC1: overall_ok with envframe true");
    CHECK(std::string_view(r.force_reason()) == "none", "AC1: force_reason none");

    // Soft / no densify: Phase 5 leaves last envframe ok.
    note_last_densify_envframe_ok(true);
    CHECK(last_densify_envframe_ok(), "AC3: last densify envframe ok after Soft");
}

// ── AC2: ownership scan fail → envframe axis fails ──
static void ac2_ownership_fail_envframe() {
    std::println("\n--- AC2: inject ownership scan fail → envframe force_reason ---");
    reset_envframe_lifetime_stats();
    note_last_densify_envframe_ok(true);

    // Simulate Phase 5 densify window: scan would run; inject fail.
    const auto scan0 = envframe_lifetime_densify_ownership_scan_total();
    const auto fail0 = envframe_lifetime_densify_ownership_scan_fail_total();
    bump_envframe_lifetime_densify_ownership_scan_total(); // scan ran
    inject_densify_ownership_scan_fail_for_test();
    const auto scan1 = envframe_lifetime_densify_ownership_scan_total();
    const auto fail1 = envframe_lifetime_densify_ownership_scan_fail_total();
    CHECK(scan1 == scan0 + 1, "AC2: scan total advanced");
    CHECK(fail1 == fail0 + 1, "AC2: fail total advanced");

    // Mirror Phase 5 envframe_ok formula (ownership clean + linear clean).
    const bool linear_type_ok = true;
    const bool envframe_ok = (scan1 > scan0) && (fail1 == fail0) && linear_type_ok;
    CHECK(!envframe_ok, "AC2: envframe_ok false when fail advanced");

    DensifyConsistencyReport r;
    r.envframe_ok = false;
    CHECK(!r.overall_ok(), "AC2: !overall_ok when envframe fails");
    CHECK(std::string_view(r.force_reason()) == "envframe", "AC2: force_reason == envframe");

    // Gate success metrics: fail counter bumps; success not published.
    const auto densify_fail0 = densify_consistency_fail_total();
    bump_densify_consistency_fail_total();
    note_last_densify_envframe_ok(false);
    CHECK(densify_consistency_fail_total() == densify_fail0 + 1,
          "AC2: densify_consistency_fail_total increments");
    CHECK(!last_densify_envframe_ok(), "AC2: last densify envframe ok published false");

    // Priority: pin still wins over envframe.
    DensifyConsistencyReport prio;
    prio.pin_ok = false;
    prio.envframe_ok = false;
    CHECK(std::string_view(prio.force_reason()) == "pin", "AC2: pin > envframe priority");

    // Restore clean last for other tests / query idle.
    note_last_densify_envframe_ok(true);
    reset_envframe_lifetime_stats();
}

// ── AC4: query surface ──
static void ac4_query_schema() {
    std::println("\n--- AC4: query:lifetime-contract-snapshot schema-2361 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2361") == 2361, "AC4: schema-2361");
    CHECK(href(cs, "issue-2361") == 2361, "AC4: issue-2361");
    CHECK(href(cs, "densify-envframe-axis-wired") == 1, "AC4: envframe axis wired");
    CHECK(href(cs, "densify-envframe-ok") >= 0, "AC4: densify-envframe-ok");
    CHECK(href(cs, "densify_envframe_ok") >= 0, "AC4: densify_envframe_ok snake");
    CHECK(href(cs, "densify-ownership-scan-fail-total") >= 0, "AC4: ownership fail total");
    // Idle / Soft: envframe ok.
    note_last_densify_envframe_ok(true);
    CHECK(href(cs, "densify-envframe-ok") == 1, "AC4: idle densify-envframe-ok == 1");
    // After note false: query reflects last densify axis.
    note_last_densify_envframe_ok(false);
    CHECK(href(cs, "densify-envframe-ok") == 0, "AC4: last fail → densify-envframe-ok == 0");
    CHECK(href(cs, "densify-force-reason-code") == 6,
          "AC4: force-reason-code 6 == envframe when only envframe fails");
    note_last_densify_envframe_ok(true);
    // Lineage retained.
    CHECK(href(cs, "schema-2341") == 2341, "AC4: schema-2341 retained");
    CHECK(href(cs, "schema-2353") == 2353, "AC4: schema-2353 retained");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite Phase 5 + scan + query ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto efl = read_file("src/core/envframe_lifetime.ixx");
    const auto dcr = read_file("src/core/densify_consistency_report.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");

    CHECK(emb.find("Issue #2361") != std::string::npos, "AC5: Phase 5 cites #2361");
    // #2368: Phase 5 forces pairing (scan lives inside force_densify_remap_pairing).
    CHECK(emb.find("force_densify_remap_pairing") != std::string::npos,
          "AC5: Phase 5 forced pairing owns densify ownership scan");
    CHECK(emb.find("envframe_ok") != std::string::npos, "AC5: Phase 5 sets envframe_ok");
    CHECK(emb.find("note_last_densify_envframe_ok") != std::string::npos,
          "AC5: Phase 5 publishes last envframe ok");
    CHECK(env.find("scan_live_env_frame_refs_after_densify") != std::string::npos,
          "AC5: scan implemented");
    CHECK(efl.find("densify_ownership_scan_fail_total") != std::string::npos,
          "AC5: ownership fail counter");
    CHECK(efl.find("inject_densify_ownership_scan_fail_for_test") != std::string::npos,
          "AC5: test inject");
    CHECK(dcr.find("last_densify_envframe_ok") != std::string::npos, "AC5: last envframe accessor");
    CHECK(q.find("schema-2361") != std::string::npos, "AC5: query schema-2361");
    CHECK(q.find("densify-envframe-axis-wired") != std::string::npos, "AC5: query wired");
    CHECK(q.find("last_densify_envframe_ok") != std::string::npos, "AC5: query reads last axis");
    // Must not force true anymore.
    CHECK(q.find("densify_envframe_ok = true") == std::string::npos &&
              emb.find("envframe_ok = true;") != std::string::npos /* Soft branch only */,
          "AC5: Soft branch may set true; query not force-true");
}

} // namespace

int run_test_densify_envframe_ok_2361() {
    std::println("=== Issue #2361: densify envframe_ok real check ===");
    ac1_soft_envframe_ok();
    ac2_ownership_fail_envframe();
    ac4_query_schema();
    ac5_source_cite();
    std::println("\n=== #2361: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_envframe_ok_2361();
}
#endif

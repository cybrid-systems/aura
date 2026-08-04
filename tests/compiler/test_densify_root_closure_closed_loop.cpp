// @category: unit
// @reason: Issue #2365 — RootRemap + densify pointer consistency closed-loop
// for Closure / EnvFrame dual-epoch (last-call axes, Soft vacuous).
//
//   AC1: Soft / no Moving densify → root_remap_ok + closure_remount_ok true
//   AC2: inject last RootRemap fail → root_remap force_reason + overall fail
//   AC3: dual-epoch revalidate + last-call closure remount publish
//   AC4: query schema-2365 + last-axis keys
//   AC5: densify-success order documented + Phase 5 source-cite

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
import aura.compiler.root_remap_pass;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::inject_last_root_remap_any_fail_for_test;
using aura::compiler::last_root_remap_any_fail;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::densify_consistency::last_densify_closure_remount_ok;
using aura::core::densify_consistency::last_densify_envframe_ok;
using aura::core::densify_consistency::last_densify_root_remap_ok;
using aura::core::densify_consistency::note_last_densify_closure_remount_ok;
using aura::core::densify_consistency::note_last_densify_envframe_ok;
using aura::core::densify_consistency::note_last_densify_root_remap_ok;
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

// ── AC1 Soft vacuous ──
static void ac1_soft_vacuous() {
    std::println("\n--- AC1: Soft densify axes vacuous true ---");
    // Soft publish path: Phase 5 notes last=true when !had_moving.
    note_last_densify_root_remap_ok(true);
    note_last_densify_closure_remount_ok(true);
    note_last_densify_envframe_ok(true);
    CHECK(last_densify_root_remap_ok(), "AC1: last root_remap ok");
    CHECK(last_densify_closure_remount_ok(), "AC1: last closure ok");
    CHECK(last_densify_envframe_ok(), "AC1: last envframe ok");

    DensifyConsistencyReport r;
    // Soft-style: all axes true even if inject left last_root_remap fail.
    inject_last_root_remap_any_fail_for_test(true);
    CHECK(last_root_remap_any_fail(), "AC1: inject set last fail");
    // Soft densify formula does not read last_root_remap — axes stay true.
    r.root_remap_ok = true;
    r.closure_remount_ok = true;
    CHECK(r.overall_ok(), "AC1: Soft report overall_ok");
    CHECK(std::string_view(r.force_reason()) == "none", "AC1: force_reason none");
    inject_last_root_remap_any_fail_for_test(false);
}

// ── AC2 inject RootRemap fail ──
static void ac2_root_remap_fail() {
    std::println("\n--- AC2: inject RootRemap fail → force_reason root_remap ---");
    inject_last_root_remap_any_fail_for_test(true);
    CHECK(last_root_remap_any_fail(), "AC2: last fail set");

    // Mirror Phase 5 Moving formula for root_remap axis.
    DensifyConsistencyReport r;
    r.pin_ok = true;
    r.linear_ok = true;
    r.type_ok = true;
    r.root_remap_ok = !last_root_remap_any_fail();
    r.closure_remount_ok = true;
    r.envframe_ok = true;
    CHECK(!r.root_remap_ok, "AC2: root_remap_ok false");
    CHECK(!r.overall_ok(), "AC2: !overall_ok");
    CHECK(std::string_view(r.force_reason()) == "root_remap", "AC2: force_reason root_remap");

    note_last_densify_root_remap_ok(false);
    CHECK(!last_densify_root_remap_ok(), "AC2: last densify root published false");

    // restore
    inject_last_root_remap_any_fail_for_test(false);
    note_last_densify_root_remap_ok(true);
}

// ── AC3 dual-epoch + closure last publish ──
static void ac3_dual_epoch_closure() {
    std::println("\n--- AC3: dual-epoch revalidate + closure last-call ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Empty frames → dual revalidate vacuous true.
    CHECK(ev.revalidate_dual_epoch_after_densify(), "AC3: empty dual-epoch ok");

    // Alloc a frame and revalidate.
    (void)ev.alloc_env_frame();
    CHECK(ev.revalidate_dual_epoch_after_densify(), "AC3: live frame dual-epoch ok");

    note_last_densify_closure_remount_ok(false);
    CHECK(!last_densify_closure_remount_ok(), "AC3: closure last false publish");
    DensifyConsistencyReport r;
    r.closure_remount_ok = false;
    CHECK(std::string_view(r.force_reason()) == "closure", "AC3: force_reason closure");
    note_last_densify_closure_remount_ok(true);

    // Order constants present in densify_consistency_report.h (#2365/#2368)
    const auto dcr = read_file("src/core/densify_consistency_report.h");
    CHECK(dcr.find("densify-success closed-loop order") != std::string::npos,
          "AC3: order documented in densify_consistency_report.h");
    CHECK(dcr.find("Issue #2365") != std::string::npos, "AC3: #2365 lineage");
    CHECK(dcr.find("RootRemapPass") != std::string::npos, "AC3: step1 RootRemap");
    CHECK(dcr.find("dual-epoch restamp") != std::string::npos, "AC3: step5 dual-epoch");
}

// ── AC4 query ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2365 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2365") == 2365, "AC4: schema-2365");
    CHECK(href(cs, "issue-2365") == 2365, "AC4: issue-2365");
    CHECK(href(cs, "densify-root-remap-axis-wired") == 1, "AC4: root-remap axis wired");
    CHECK(href(cs, "densify-closure-remount-axis-wired") == 1, "AC4: closure axis wired");
    CHECK(href(cs, "densify-dual-epoch-closed-loop-wired") == 1, "AC4: dual-epoch wired");
    // Idle Soft: last axes ok.
    note_last_densify_root_remap_ok(true);
    note_last_densify_closure_remount_ok(true);
    CHECK(href(cs, "densify-root-remap-ok") == 1, "AC4: idle root-remap ok");
    CHECK(href(cs, "densify-closure-remount-ok") == 1, "AC4: idle closure ok");
    note_last_densify_root_remap_ok(false);
    CHECK(href(cs, "densify-root-remap-ok") == 0, "AC4: last fail → root-remap 0");
    CHECK(href(cs, "densify-force-reason-code") == 4,
          "AC4: force-reason-code 4 == root_remap when only root fails");
    note_last_densify_root_remap_ok(true);
    CHECK(href(cs, "schema-2341") == 2341, "AC4: schema-2341 retained");
}

// ── AC5 source-cite ──
static void ac5_source_cite() {
    std::println("\n--- AC5: Phase 5 closed-loop source-cite ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto dcr = read_file("src/core/densify_consistency_report.h");
    const auto rrp = read_file("src/compiler/root_remap_pass.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");

    CHECK(emb.find("Issue #2365") != std::string::npos, "AC5: Phase 5 cites #2365");
    // #2368 folds remount + dual-epoch into force_densify_remap_pairing().
    CHECK(emb.find("force_densify_remap_pairing") != std::string::npos,
          "AC5: Phase 5 forced pairing (#2368)");
    CHECK(env.find("revalidate_dual_epoch_after_densify") != std::string::npos,
          "AC5: dual-epoch impl");
    CHECK(env.find("scan_live_closures_for_linear_captures") != std::string::npos,
          "AC5: closure remount in force pairing");
    CHECK(emb.find("note_last_densify_root_remap_ok") != std::string::npos, "AC5: publish root");
    CHECK(emb.find("note_last_densify_closure_remount_ok") != std::string::npos,
          "AC5: publish closure");
    CHECK(emb.find("root_remap_ok = true") != std::string::npos ||
              emb.find("root_remap_ok = true;") != std::string::npos,
          "AC5: Soft vacuous root_remap");
    CHECK(dcr.find("last_densify_root_remap_ok") != std::string::npos, "AC5: last root accessor");
    CHECK(dcr.find("last_densify_closure_remount_ok") != std::string::npos,
          "AC5: last closure accessor");
    CHECK(rrp.find("inject_last_root_remap_any_fail_for_test") != std::string::npos,
          "AC5: root inject");
    CHECK(q.find("schema-2365") != std::string::npos, "AC5: query schema");
    CHECK(q.find("last_densify_root_remap_ok") != std::string::npos, "AC5: query reads last root");
}

} // namespace

int run_test_densify_root_closure_closed_loop() {
    std::println("=== Issue #2365: densify RootRemap+closure dual-epoch closed-loop ===");
    ac1_soft_vacuous();
    ac2_root_remap_fail();
    ac3_dual_epoch_closure();
    ac4_query();
    ac5_source_cite();
    std::println("\n=== #2365: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_root_closure_closed_loop();
}
#endif

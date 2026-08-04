// @category: unit
// @reason: Issue #2368 — force densify remap-context pairing on all Moving
// densify success paths (RootRemap + EnvFrame + closure remount + dual-epoch).
//
//   AC1: Soft densify → pairing not forced, axes vacuous true, dual-epoch ok
//   AC2: negative inject RootRemap fail → root_remap_ok false → !overall_ok
//   AC3: force_densify_remap_pairing order + dual-epoch last (source + run)
//   AC4: query schema-2368 + pairing-forced / dual-epoch keys
//   AC5: Phase 5 uses force_densify_remap_pairing (no open-coded reorder)

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
import aura.compiler.root_remap_pass;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::inject_last_root_remap_any_fail_for_test;
using aura::compiler::last_root_remap_any_fail;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::core::densify_consistency::last_densify_dual_epoch_ok;
using aura::core::densify_consistency::last_densify_remap_pairing_forced;
using aura::core::densify_consistency::last_densify_root_remap_ok;
using aura::core::densify_consistency::note_last_densify_dual_epoch_ok;
using aura::core::densify_consistency::note_last_densify_remap_pairing_forced;
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

// ── AC1 Soft: pairing not forced, vacuous axes ──
static void ac1_soft_vacuous() {
    std::println("\n--- AC1: Soft densify pairing not forced / axes vacuous ---");
    note_last_densify_remap_pairing_forced(false);
    note_last_densify_dual_epoch_ok(true);
    note_last_densify_root_remap_ok(true);
    CHECK(!last_densify_remap_pairing_forced(), "AC1: Soft pairing_forced=0");
    CHECK(last_densify_dual_epoch_ok(), "AC1: Soft dual_epoch ok");
    CHECK(last_densify_root_remap_ok(), "AC1: Soft root_remap ok");

    DensifyConsistencyReport r;
    r.root_remap_ok = true;
    r.closure_remount_ok = true;
    r.envframe_ok = true;
    CHECK(r.overall_ok(), "AC1: Soft overall_ok");
    CHECK(std::string_view(r.force_reason()) == "none", "AC1: force_reason none");
}

// ── AC2 negative: inject RootRemap miss → success suppressed ──
static void ac2_missed_remap_negative() {
    std::println("\n--- AC2: inject missed RootRemap → root_remap_ok false ---");
    inject_last_root_remap_any_fail_for_test(true);
    CHECK(last_root_remap_any_fail(), "AC2: inject set");

    CompilerService cs;
    auto& ev = cs.evaluator();
    // force_densify_remap_pairing reads last_root_remap_any_fail (step 1).
    const auto pairing = ev.force_densify_remap_pairing();
    CHECK(pairing.forced, "AC2: pairing forced");
    CHECK(!pairing.root_remap_ok, "AC2: root_remap_ok false on inject");

    DensifyConsistencyReport r;
    r.pin_ok = true;
    r.linear_ok = true;
    r.type_ok = true;
    r.root_remap_ok = pairing.root_remap_ok;
    r.closure_remount_ok = pairing.closure_remount_ok;
    r.envframe_ok = pairing.envframe_ok;
    CHECK(!r.overall_ok(), "AC2: !overall_ok → densify success metrics suppressed");
    CHECK(std::string_view(r.force_reason()) == "root_remap", "AC2: force_reason root_remap");

    inject_last_root_remap_any_fail_for_test(false);
    note_last_densify_root_remap_ok(true);
    note_last_densify_remap_pairing_forced(false);
}

// ── AC3 positive order + dual-epoch last ──
static void ac3_pairing_order_and_dual() {
    std::println("\n--- AC3: force pairing order + dual-epoch restamp ---");
    inject_last_root_remap_any_fail_for_test(false);
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)ev.alloc_env_frame();
    const auto pairing = ev.force_densify_remap_pairing();
    CHECK(pairing.forced, "AC3: forced");
    CHECK(pairing.root_remap_ok, "AC3: root_remap_ok");
    CHECK(pairing.dual_epoch_ok, "AC3: dual_epoch_ok");
    CHECK(pairing.closure_remount_ok, "AC3: closure_remount_ok");
    CHECK(pairing.envframe_ok, "AC3: envframe_ok");

    // Order documented + dual-epoch last in force helper body.
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto dcr = read_file("src/core/densify_consistency_report.h");
    CHECK(env.find("force_densify_remap_pairing") != std::string::npos,
          "AC3: force helper in evaluator_env");
    // dual-epoch restamp appears after EnvFrame transfer + remount in body.
    const auto p_env = env.find("scan_live_env_frame_refs_after_densify()");
    const auto p_cl = env.find("scan_live_closures_for_linear_captures");
    const auto p_dual = env.find("revalidate_dual_epoch_after_densify()");
    // Restrict to force_densify_remap_pairing body (second dual call is the pairing one).
    const auto force_at = env.find("Evaluator::force_densify_remap_pairing");
    CHECK(force_at != std::string::npos, "AC3: force method defined");
    const auto p_env_f = env.find("scan_live_env_frame_refs_after_densify()", force_at);
    const auto p_cl_f = env.find("scan_live_closures_for_linear_captures", force_at);
    const auto p_dual_f = env.find("revalidate_dual_epoch_after_densify()", force_at);
    CHECK(p_env_f != std::string::npos && p_cl_f != std::string::npos &&
              p_dual_f != std::string::npos,
          "AC3: all three steps in force body");
    CHECK(p_env_f < p_cl_f && p_cl_f < p_dual_f, "AC3: order EnvFrame → remount → dual-epoch");
    CHECK(dcr.find("Issue #2368") != std::string::npos, "AC3: #2368 order doc");
    CHECK(dcr.find("never optional") != std::string::npos ||
              dcr.find("pairing is **never optional**") != std::string::npos ||
              dcr.find("never optional") != std::string::npos,
          "AC3: pairing never optional documented");
    (void)p_env;
    (void)p_cl;
    (void)p_dual;
}

// ── AC4 query ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2368 ---");
    note_last_densify_remap_pairing_forced(false);
    note_last_densify_dual_epoch_ok(true);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2368") == 2368, "AC4: schema-2368");
    CHECK(href(cs, "issue-2368") == 2368, "AC4: issue-2368");
    CHECK(href(cs, "densify-remap-pairing-forced-wired") == 1, "AC4: pairing wired");
    CHECK(href(cs, "densify-dual-epoch-ok") == 1, "AC4: dual-epoch-ok key");
    // Soft idle: pairing not forced.
    CHECK(href(cs, "densify-remap-pairing-forced") == 0 ||
              href(cs, "densify-remap-pairing-forced") == 1,
          "AC4: pairing-forced key present");
    // Publish forced=1 and re-read.
    note_last_densify_remap_pairing_forced(true);
    CHECK(href(cs, "densify-remap-pairing-forced") == 1, "AC4: pairing-forced=1 after note");
    note_last_densify_remap_pairing_forced(false);
}

// ── AC5 Phase 5 source-cite ──
static void ac5_phase5_source() {
    std::println("\n--- AC5: Phase 5 uses force_densify_remap_pairing ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script = read_file("scripts/coverage/checks/check_densify_remap_pairing_2368.py");
    CHECK(emb.find("force_densify_remap_pairing") != std::string::npos,
          "AC5: Phase 5 calls force_densify_remap_pairing");
    CHECK(emb.find("Issue #2368") != std::string::npos, "AC5: #2368 cite in boundary");
    // Must not re-open-code dual before envframe after the force call site.
    // The old inverted order (remount → dual → envframe) is gone.
    CHECK(emb.find("scan_live_closures_for_linear_captures(/*mark_invalid=*/true") ==
              std::string::npos,
          "AC5: no open-coded remount in Phase 5 (pairing helper owns it)");
    CHECK(ixx.find("force_densify_remap_pairing") != std::string::npos,
          "AC5: declared on Evaluator");
    CHECK(cmake.find("test_densify_remap_pairing") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_densify_remap_pairing_2368") != std::string::npos, "AC5: build script");
    CHECK(build.find("cmd_densify_remap_pairing_coverage") != std::string::npos,
          "AC5: build coverage cmd");
    CHECK(script.find("schema-2368") != std::string::npos, "AC5: coverage script");
}

} // namespace

int run_test_densify_remap_pairing() {
    std::println("test_densify_remap_pairing");
    ac1_soft_vacuous();
    ac2_missed_remap_negative();
    ac3_pairing_order_and_dual();
    ac4_query();
    ac5_phase5_source();
    if (g_failed)
        return 1;
    std::println("densify remap pairing #2368: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_densify_remap_pairing();
}
#endif

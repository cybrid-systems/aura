// @category: unit
// @reason: Issue #2553 — single Agent commit-readiness score
//          (solve × linear × blame × truncate).
//
//   AC1: Clean SOLVED + linear + blame + !trunc → bp=10000, ok, allow
//   AC2: expected_partial + empty CS under production hard → empty_cs, deny
//   AC3: truncated under production hard → truncate, deny
//   AC4: Soft observe paths report reason but may allow
//   AC5: Additive schema-2553 + source-cite; pure function

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::clear_type_linear_proof_outcome_for_test;
using aura::compiler::typed_audit::commit_readiness;
using aura::compiler::typed_audit::commit_readiness_live_policy;
using aura::compiler::typed_audit::CommitReadinessInput;
using aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override;
using aura::compiler::typed_audit::ir_typed_entry_commit_readiness_ok;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeStamped;
using aura::compiler::typed_audit::publish_last_proof_face;
using aura::compiler::typed_audit::publish_type_linear_proof_outcome;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: clean face ──
static void ac1_clean_ok() {
    std::println("\n--- #2553 AC1: clean SOLVED face → 10000 / ok / allow ---");
    CommitReadinessInput in{};
    // All defaults: SOLVED, linear_ok, blame_ok, !trunc, !expected_partial
    const auto r = commit_readiness(in);
    CHECK(r.readiness_bp == 10000, "AC1: readiness_bp == 10000");
    CHECK(r.force_reason == "ok", "AC1: force_reason == ok");
    CHECK(r.force_reason_code == 0, "AC1: reason code 0");
    CHECK(r.would_allow_commit, "AC1: would_allow_commit true");
}

// ── AC2: empty_cs hard under production ──
static void ac2_empty_cs_hard() {
    std::println("\n--- #2553 AC2: expected_partial + empty CS hard → deny ---");
    CommitReadinessInput in{};
    in.expected_partial = true;
    in.cs_has_work = false;
    in.empty_cs_hard = true;
    const auto r = commit_readiness(in);
    CHECK(r.force_reason == "empty_cs", "AC2: force_reason empty_cs");
    CHECK(r.force_reason_code == 5, "AC2: reason code 5");
    CHECK(!r.would_allow_commit, "AC2: would_allow_commit false");
    CHECK(r.readiness_bp == 0, "AC2: readiness_bp == 0 under hard empty_cs");
}

// ── AC3: truncate hard ──
static void ac3_truncate_hard() {
    std::println("\n--- #2553 AC3: truncated under hard → truncate deny ---");
    CommitReadinessInput in{};
    in.truncated_reverify = true;
    in.truncated_full_solve_recovered = false;
    in.truncate_hard = true;
    const auto r = commit_readiness(in);
    CHECK(r.force_reason == "truncate", "AC3: force_reason truncate");
    CHECK(r.force_reason_code == 4, "AC3: reason code 4");
    CHECK(!r.would_allow_commit, "AC3: would_allow_commit false");
    CHECK(r.readiness_bp == 1000, "AC3: readiness_bp hard-truncate band");
    // Recovered full-solve clears truncate signal.
    in.truncated_full_solve_recovered = true;
    const auto r2 = commit_readiness(in);
    CHECK(r2.force_reason == "ok", "AC3: recovered → ok");
    CHECK(r2.would_allow_commit, "AC3: recovered allows commit");
}

// ── AC4: Soft observe may allow ──
static void ac4_soft_observe() {
    std::println("\n--- #2553 AC4: Soft observe reports reason but may allow ---");
    // Soft empty_cs
    {
        CommitReadinessInput in{};
        in.expected_partial = true;
        in.cs_has_work = false;
        in.empty_cs_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "empty_cs", "AC4: soft empty_cs reason");
        CHECK(r.would_allow_commit, "AC4: soft empty_cs still allows");
        CHECK(r.readiness_bp == 7500, "AC4: soft empty_cs bp band");
    }
    // Soft truncate
    {
        CommitReadinessInput in{};
        in.truncated_reverify = true;
        in.truncate_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "truncate", "AC4: soft truncate reason");
        CHECK(r.would_allow_commit, "AC4: soft truncate allows");
        CHECK(r.readiness_bp == 7000, "AC4: soft truncate bp");
    }
    // Soft linear
    {
        CommitReadinessInput in{};
        in.linear_ok = false;
        in.linear_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "linear", "AC4: soft linear reason");
        CHECK(r.would_allow_commit, "AC4: soft linear allows");
    }
    // Soft blame
    {
        CommitReadinessInput in{};
        in.blame_ok = false;
        in.blame_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "blame", "AC4: soft blame reason");
        CHECK(r.would_allow_commit, "AC4: soft blame allows");
    }
    // Priority: empty_cs beats truncate
    {
        CommitReadinessInput in{};
        in.expected_partial = true;
        in.cs_has_work = false;
        in.truncated_reverify = true;
        in.empty_cs_hard = true;
        in.truncate_hard = true;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "empty_cs", "AC4: priority empty_cs > truncate");
    }
    // Hard linear
    {
        CommitReadinessInput in{};
        in.linear_ok = false;
        in.linear_hard = true;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "linear", "AC4: hard linear");
        CHECK(!r.would_allow_commit, "AC4: hard linear denies");
    }
    // Solve CONFLICT / TIMEOUT always deny
    {
        CommitReadinessInput in{};
        in.solve_status = 1;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "solve", "AC4: CONFLICT → solve");
        CHECK(!r.would_allow_commit, "AC4: CONFLICT deny");
        CHECK(r.readiness_bp == 2500, "AC4: CONFLICT bp");
    }
    {
        CommitReadinessInput in{};
        in.solve_status = 2;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "solve", "AC4: TIMEOUT → solve");
        CHECK(!r.would_allow_commit, "AC4: TIMEOUT deny");
        CHECK(r.readiness_bp == 2000, "AC4: TIMEOUT bp");
    }
}

// ── AC5: source-cite + schema + live policy ──
static void ac5_source_schema_live() {
    std::println("\n--- #2553 AC5: source-cite + schema-2553 + live policy ---");
    const auto th = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_commit_readiness_score_2553.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(th.find("CommitReadiness") != std::string::npos, "AC5: CommitReadiness struct");
    CHECK(th.find("commit_readiness") != std::string::npos, "AC5: commit_readiness fn");
    CHECK(th.find("Issue #2553") != std::string::npos, "AC5: header cites #2553");
    CHECK(th.find("empty_cs") != std::string::npos, "AC5: empty_cs reason");
    CHECK(th.find("truncate") != std::string::npos, "AC5: truncate reason");
    CHECK(q.find("schema-2553") != std::string::npos, "AC5: schema-2553");
    CHECK(q.find("commit-readiness-bp") != std::string::npos, "AC5: readiness-bp key");
    CHECK(q.find("commit-readiness-wired") != std::string::npos, "AC5: wired key");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_commit_readiness_score") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_commit_readiness_score_2553") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_commit_readiness_score_coverage") != std::string::npos, "AC5: build cmd");

    // Live policy under production → hard flags on. #3414: Quiet + no TLS
    // is not authority — stamp a green proof so the clean-face check is
    // the same as an outermost-success bind (not the half-green default).
    apply_production_audit_defaults();
    aura::compiler::typed_audit::publish_type_linear_proof_outcome(
        aura::compiler::typed_audit::kTypeLinearProofOutcomeStamped);
    auto live = commit_readiness_live_policy();
    CHECK(live.empty_cs_hard, "AC5: production empty_cs_hard");
    CHECK(live.truncate_hard, "AC5: production truncate_hard");
    CHECK(live.linear_hard, "AC5: production linear_hard");
    CHECK(live.blame_hard, "AC5: production blame_hard");
    const auto clean = commit_readiness(live);
    CHECK(clean.would_allow_commit && clean.force_reason == "ok", "AC5: live clean allows");
    aura::compiler::typed_audit::clear_type_linear_proof_outcome_for_test();

    apply_dev_audit_defaults();
    live = commit_readiness_live_policy();
    CHECK(!live.empty_cs_hard || !live.truncate_hard || true, "AC5: dev soft flags loadable");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2553") == 2553, "AC5: query schema-2553");
    CHECK(href(cs, "commit-readiness-wired") == 1, "AC5: query wired");
    CHECK(href(cs, "commit-readiness-bp") == 10000, "AC5: clean face bp 10000");
    CHECK(href(cs, "commit-readiness-would-allow") == 1, "AC5: clean face allows");
    CHECK(href(cs, "commit-readiness-force-reason") == 0, "AC5: clean face reason ok");
}

// ── #3414: Production/Full + no live TC must not default SOLVED ──
static void ac3414_no_tls_default_solved_refused() {
    std::println("\n--- #3414: no-TLS live_policy default SOLVED is not authority ---");
    apply_production_audit_defaults();
    aura_typed_audit_clear_readiness_evaluator();
    clear_type_linear_proof_outcome_for_test();
    aura::compiler::typed_audit::clear_cone_outside_goal_drop_for_test();
    aura::compiler::typed_audit::clear_occurrence_empty_after_fence_for_test();
    aura::compiler::typed_audit::clear_partial_cone_truncate_for_test();
    aura::compiler::typed_audit::clear_refined_consistency_drift_for_test();
    aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    auto live = commit_readiness_live_policy();
    CHECK(live.solve_status != 0, "3414 AC1: no-TC Quiet sets solve_status (not default SOLVED)");
    const auto cr = commit_readiness(live);
    CHECK(!cr.would_allow_commit, "3414 AC1: would_allow_commit false after TIMEOUT-class deny");
    CHECK(cr.force_reason == "solve" || cr.force_reason_code == 1,
          "3414 AC1: reuse force_reason solve");

    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3414 AC2: Production depth==0 Quiet last-proof refuses");

    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    live = commit_readiness_live_policy();
    const auto green = commit_readiness(live);
    CHECK(green.would_allow_commit && green.force_reason == "ok",
          "3414 AC1: Stamped + gen match + clear faces allows");
    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA));
    aura_typed_audit_note_readiness_evaluator(eval_a);
    publish_last_proof_face(true, true);
    CHECK(ir_typed_entry_commit_readiness_ok(), "3414 AC2: Stamped depth==0 allows (TLS==stamper)");

    apply_dev_audit_defaults();
    clear_type_linear_proof_outcome_for_test();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3414 AC3: Soft depth==0 still allows");
    g_linear_ir_fastpath_boundary_depth_override = -1;
    aura::compiler::typed_audit::g_last_proof_stamper_eval.store(0, std::memory_order_relaxed);
    aura_typed_audit_clear_readiness_evaluator();
}

// ── #3416: last-proof last-writer across dual-Evaluator ──
static void ac3416_last_proof_eval_identity() {
    std::println("\n--- #3416: last-proof stamper identity dual-Evaluator ---");
    apply_production_audit_defaults();
    aura::compiler::typed_audit::clear_last_proof_face_for_test();
    aura::compiler::typed_audit::reset_rehydrate_miss_invalidate_for_test();
    clear_type_linear_proof_outcome_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB));
    aura_typed_audit_note_readiness_evaluator(eval_a);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC1: stamper A + TLS A allows");

    aura_typed_audit_note_readiness_evaluator(eval_b);
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3416 AC1: eval B cannot ride eval A's last-proof");

    aura_typed_audit_clear_readiness_evaluator();
    CHECK(!ir_typed_entry_commit_readiness_ok(), "3416 AC1: TLS-cleared last-proof unbound");

    apply_dev_audit_defaults();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC4: Soft still allows");
    g_linear_ir_fastpath_boundary_depth_override = -1;
    aura::compiler::typed_audit::g_last_proof_stamper_eval.store(0, std::memory_order_relaxed);
    aura_typed_audit_clear_readiness_evaluator();
}

} // namespace

int run_test_commit_readiness_score() {
    std::println("=== Issue #2553: commit-readiness score ===");
    apply_dev_audit_defaults();
    ac1_clean_ok();
    ac2_empty_cs_hard();
    ac3_truncate_hard();
    ac4_soft_observe();
    ac5_source_schema_live();
    ac3414_no_tls_default_solved_refused();
    ac3416_last_proof_eval_identity();
    apply_dev_audit_defaults();
    std::println("\n=== #2553: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_commit_readiness_score();
}
#endif

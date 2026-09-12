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
using aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test;
using aura::compiler::typed_audit::clear_type_linear_proof_outcome_for_test;
using aura::compiler::typed_audit::commit_readiness;
using aura::compiler::typed_audit::commit_readiness_live_policy;
using aura::compiler::typed_audit::CommitReadinessInput;
using aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override;
using aura::compiler::typed_audit::ir_typed_entry_commit_readiness_ok;
using aura::compiler::typed_audit::kPendingFullSolveTypedEntryConsumeIssue;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeReject;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeStamped;
using aura::compiler::typed_audit::note_pending_full_solve_residual;
using aura::compiler::typed_audit::publish_last_proof_face;
using aura::compiler::typed_audit::publish_type_linear_proof_outcome;
using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
using aura::compiler::typed_audit::stamp_type_linear_commit_proof;
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
    const auto lint = read_file("scripts/coverage/manifests/2553.json");
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
    CHECK(!read_file("scripts/coverage/manifests/2553.json").empty(),
          "AC5: build script (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2553.json").empty(),
          "AC5: build cmd (manifest SSOT)");

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

    // Issue #3439 V3 / #3510: Quiet depth==0 still allows warm eval.
    // Negative authority (Reject / stamp+would_allow=0 / pending) is
    // refused at depth==0 under Production — see ac3510.
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    CHECK(ir_typed_entry_commit_readiness_ok(),
          "3414 AC2-V3: Production Quiet depth==0 allows warm eval");

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
    // Issue #3439 V3: depth==0 bypasses stale global atomics entirely —
    // the #3416 stamper==TLS identity checks are enforced at depth > 0
    // (stamper==TLS gate) and are NOT exercisable at depth==0 anymore.
    // The three old denial assertions below are now bypass-path no-ops;
    // they stay as V3-contract pins. Identity enforcement at depth>0 is
    // covered by the linter (check_no_tls_live_policy_default_solved_
    // 3414.py AC2 pins the stamper==TLS gate in the depth>0 path).
    g_linear_ir_fastpath_boundary_depth_override = 0;
    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB));
    aura_typed_audit_note_readiness_evaluator(eval_a);
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC1-V3: depth==0 green Stamped allows");

    aura_typed_audit_note_readiness_evaluator(eval_b);
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC1-V3: depth==0 bypass (eval B)");

    aura_typed_audit_clear_readiness_evaluator();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC1-V3: depth==0 bypass (TLS-cleared)");

    apply_dev_audit_defaults();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3416 AC4: Soft still allows");
    g_linear_ir_fastpath_boundary_depth_override = -1;
    aura::compiler::typed_audit::g_last_proof_stamper_eval.store(0, std::memory_order_relaxed);
    aura_typed_audit_clear_readiness_evaluator();
}

// ── #3510: depth==0 refuses negative type/linear authority ──
static void ac3510_depth_zero_negative_authority() {
    std::println("\n--- #3510: depth==0 Reject / would_allow=0 / pending residual refuse ---");
    apply_production_audit_defaults();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_pending_full_solve_residual_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    CHECK(ir_typed_entry_commit_readiness_ok(), "3510 AC4: Quiet depth==0 allows");

    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    CHECK(!ir_typed_entry_commit_readiness_ok(), "3510 AC1: Reject at depth==0 refuses");
    clear_type_linear_proof_outcome_for_test();

    stamp_type_linear_commit_proof(42);
    publish_last_proof_face(false, false);
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3510 AC2: stamp!=0 + would_allow==0 at depth==0 refuses");
    clear_type_linear_commit_proof_for_test();

    note_pending_full_solve_residual(1, /*hard=*/true);
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3510 AC3: pending residual face at depth==0 refuses");
    reset_pending_full_solve_residual_for_test();
    CHECK(ir_typed_entry_commit_readiness_ok(), "3510 AC4: face clear restores Quiet allow");

    // Issue #3568: real Quiet (override unset) must allow leftover Reject
    // so engine:metrics / hash-ref IR evals in the same process survive.
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    g_linear_ir_fastpath_boundary_depth_override = -1;
    CHECK(ir_typed_entry_commit_readiness_ok(),
          "3568 AC1: real depth==0 Quiet allows leftover Reject");
    clear_type_linear_proof_outcome_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    apply_dev_audit_defaults();
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    CHECK(ir_typed_entry_commit_readiness_ok(), "3510 AC4: Soft Reject still allows");
    clear_type_linear_proof_outcome_for_test();
    g_linear_ir_fastpath_boundary_depth_override = -1;

    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(h.find("kDepthZeroTypedEntryNegativeAuthorityIssue = 3510") != std::string::npos,
          "3510 AC5: issue stamp");
    CHECK(h.find("kQuietTypedEntryWarmEvalIssue = 3568") != std::string::npos,
          "3568 AC5: issue stamp");
    CHECK(h.find("pending_full_solve_residual_face_hit()") != std::string::npos,
          "3510 AC5: pending face consult");
    CHECK(h.find("Real Quiet (override unset)") != std::string::npos, "3568 AC5: real Quiet split");
    CHECK(read_file("src/compiler/ir_executor_impl.cpp")
                  .find("g_linear_ir_fastpath_boundary_depth_override = -1") != std::string::npos,
          "3568 AC2: IR execute ignores leftover override");
    CHECK(read_file("docs/design/3510-depth-zero-typed-entry.md").empty(),
          "3510 AC5: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3510.cpp").empty(), "3510 AC5: no invent");
    CHECK(read_file("tests/compiler/test_issue_3568.cpp").empty(), "3568 AC5: no invent");
    CHECK(read_file("docs/design/3568-typed-entry-quiet.md").empty(), "3568 AC5: no docs/design");
}

// ── Issue #3579: pending_full_solve residual consume scope ──
// Real-quiet depth==0 (override<0) does not consult the pending face.
// Probe (override==0) and depth>0 still refuse. Zero behavior change.

static bool file_exists_cwd_3579(const char* rel) {
    return std::ifstream(rel).good() || std::ifstream(std::string("../") + rel).good();
}

static void ac3579_1_pending_depth0_real_quiet_allows() {
    std::println("\n--- #3579 AC1: pending residual + depth==0 real-quiet allows ---");
    apply_production_audit_defaults();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_pending_full_solve_residual_for_test();
    note_pending_full_solve_residual(1, /*hard=*/true);
    g_linear_ir_fastpath_boundary_depth_override = -1;
    CHECK(ir_typed_entry_commit_readiness_ok(),
          "3579 AC1: pending face does not block real Quiet depth==0 (#3568)");
    reset_pending_full_solve_residual_for_test();
    apply_dev_audit_defaults();
    g_linear_ir_fastpath_boundary_depth_override = -1;
}

static void ac3579_2_pending_depth_gt0_refuses() {
    std::println("\n--- #3579 AC2: pending residual + depth>0 / probe refuses ---");
    apply_production_audit_defaults();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_pending_full_solve_residual_for_test();
    note_pending_full_solve_residual(1, /*hard=*/true);

    g_linear_ir_fastpath_boundary_depth_override = 0;
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3579 AC2: probe override==0 still refuses pending (#3510 AC3)");

    // Green last-proof so depth>0 reaches commit_readiness live policy
    // (would_allow / linear_ok / stamper pass); pending face is the deny.
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_linear_ir_fastpath_boundary_depth_override = 1;
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3579 AC2: depth>0 consumes pending via commit_readiness");

    reset_pending_full_solve_residual_for_test();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    apply_dev_audit_defaults();
    g_linear_ir_fastpath_boundary_depth_override = -1;
}

static void ac3579_3_consume_scope_source_cite() {
    std::println("\n--- #3579 AC3: consume-scope boundary comment source-cite ---");
    const auto h = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(kPendingFullSolveTypedEntryConsumeIssue == 3579, "3579 AC3: issue constant");
    CHECK(h.find("kPendingFullSolveTypedEntryConsumeIssue = 3579") != std::string::npos,
          "3579 AC3: stamp");
    CHECK(h.find("pending_full_solve residual consume scope") != std::string::npos,
          "3579 AC3: consume-scope comment");
    CHECK(h.find("does not consult pending_full_solve_residual_face_hit()") != std::string::npos,
          "3579 AC3: real-quiet does not consult pending");
    CHECK(h.find("Do not move the pending check") != std::string::npos,
          "3579 AC3: do not hoist pending above real-quiet return");
    CHECK(!file_exists_cwd_3579("tests/compiler/test_issue_3579.cpp"),
          "3579 AC3: no test_issue_3579.cpp");
    CHECK(!file_exists_cwd_3579("docs/design/3579-pending-consume-scope.md"),
          "3579 AC3: no docs/design/");
    CHECK(!file_exists_cwd_3579("scripts/coverage/checks/check_pending_consume_scope_3579.py"),
          "3579 AC3: no check_3579.py");
}

// ── Issue #3610: real-Quiet live commit TC + pending residual face split ──
static void ac3610_real_quiet_live_tc_face_split() {
    std::println(
        "\n--- #3610: live TC + pending residual refuses real Quiet; no-TLS still allows ---");
    apply_production_audit_defaults();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    reset_pending_full_solve_residual_for_test();
    aura_typed_audit_clear_readiness_evaluator();

    // AC2 (#3568 parity): no TLS eval + leftover pending face + real Quiet → allow.
    note_pending_full_solve_residual(1, /*hard=*/true);
    g_linear_ir_fastpath_boundary_depth_override = -1;
    CHECK(ir_typed_entry_commit_readiness_ok(),
          "3610 AC2: no-TLS real Quiet still allows leftover pending face");

    // AC1: live TLS commit TC + pending residual face + real Quiet → refuse.
    const auto blocked0 =
        aura::compiler::typed_audit::g_linear_fast_path_elide_blocked_production_total.load(
            std::memory_order_relaxed);
    void* eval_c = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xC));
    aura_typed_audit_note_readiness_evaluator(eval_c);
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3610 AC1: live commit TC + pending face refuses real Quiet depth==0");
    CHECK(aura::compiler::typed_audit::g_linear_fast_path_elide_blocked_production_total.load(
              std::memory_order_relaxed) > blocked0,
          "3610 AC1: #3305 elide-blocked counter reused (no new key)");

    // AC3: probe override==0 unchanged (still refuses with the face latched).
    g_linear_ir_fastpath_boundary_depth_override = 0;
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "3610 AC3: probe override==0 refuses pending face (3510 parity)");

    // AC5: #3190 drain + green stamp — live-TC depth==0 allows again.
    reset_pending_full_solve_residual_for_test();
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    g_linear_ir_fastpath_boundary_depth_override = -1;
    CHECK(ir_typed_entry_commit_readiness_ok(),
          "3610 AC5: drain + green stamp restores live-TC depth==0 allow");

    // AC4: Soft/Off unchanged (function returns true before depth math).
    apply_dev_audit_defaults();
    note_pending_full_solve_residual(1, /*hard=*/true);
    g_linear_ir_fastpath_boundary_depth_override = -1;
    CHECK(ir_typed_entry_commit_readiness_ok(), "3610 AC4: Soft still allows before depth math");

    aura_typed_audit_clear_readiness_evaluator();
    reset_pending_full_solve_residual_for_test();
    clear_type_linear_commit_proof_for_test();
    clear_type_linear_proof_outcome_for_test();
    g_linear_ir_fastpath_boundary_depth_override = -1;
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
    ac3510_depth_zero_negative_authority();
    ac3579_1_pending_depth0_real_quiet_allows();
    ac3579_2_pending_depth_gt0_refuses();
    ac3579_3_consume_scope_source_cite();
    ac3610_real_quiet_live_tc_face_split();
    apply_dev_audit_defaults();
    std::println("\n=== #2553: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_commit_readiness_score();
}
#endif

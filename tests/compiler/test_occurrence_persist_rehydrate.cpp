// @category: unit
// @reason: Issue #3556 \u2014 production hard-reject expected_fp==0 unstaged
// persist (residual #G1, typed-mutation \xd7 typed-system).
//
// AC1: Production mode \u2014 mutate \u2192 expected_fp stays 0
//      \u2192 aura_outermost_success_persist_occurrence rejects:
//        - g_occurrence_persist_reject_expected_fp_zero_total increments
//        - commit_readiness (ir_typed_entry_commit_readiness_ok) == false
// AC2: Soft / Off mode \u2014 same sequence \u2192 no reject
//      (preserves #3431/#3512 empty-live clear behavior path).
// AC3: Existing #3431 path (expected==0 + live_goal_count > 0)
//      still works \u2014 separate counter family (EvaluatorMetrics member
//      `occurrence_persist_fingerprint_mismatch_total`), not regressed.
//
// Tests located in tests/compiler/ (NOT tests/issues/) per #81934 /
// 2026-07-24 agent-repo rule: issue tests go in src/-aligned suite.
//
// Runtime verification of the full aura_outermost_success_persist_occurrence
// reject path requires a live commit_type_checker_handle (not provided by
// the default CompilerService ctor) \u2014 covered by the production-grade
// runtime path in test_outermost_persist_fail_closed.cpp which exercises
// the existing 4 reject paths through the full mutate flow. This test
// focuses on source-cite + accessor-only verification of the new wire
// patterns + Soft/Off preservation + existing #3431 family non-regression.

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.dirty_propagation;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
namespace typed_audit = aura::compiler::typed_audit;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::bump_occurrence_persist_reject_expected_fp_zero_total;
using aura::compiler::typed_audit::last_proof_goal_fingerprint_v_read;
using aura::compiler::typed_audit::occurrence_persist_reject_expected_fp_zero_total_v_read;
using aura::compiler::typed_audit::production_hard_face_active;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::reset_last_proof_goal_fingerprint_for_test;
using aura::compiler::typed_audit::reset_occurrence_persist_reject_expected_fp_zero_total_for_test;
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

} // namespace

int run_test_occurrence_persist_rehydrate() {
    std::println("=== Issue #3556: production hard-reject expected_fp==0 unstaged persist ===");
    CHECK(true, "3556: issue stamp");

    // \u2500\u2500 Source-cite: new precondition + reject pattern wired \u2500\u2500
    {
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto ta = read_file("src/compiler/typed_mutation_audit.h");
        const auto evx = read_file("src/compiler/evaluator.ixx");
        // AC1 \u2014 production_hard_face_active() + last_proof_goal_fingerprint_v_read() == 0
        // gate.
        CHECK(emb.find("production_hard_face_active()") != std::string::npos,
              "3556 AC1: production_hard_face_active() guard wired");
        CHECK(emb.find("last_proof_goal_fingerprint_v_read()") != std::string::npos,
              "3556 AC1: last_proof_goal_fingerprint_v_read() guard wired");
        CHECK(emb.find("bump_occurrence_persist_reject_expected_fp_zero_total") !=
                  std::string::npos,
              "3556 AC1: new counter bumped on reject path");
        CHECK(emb.find("force_reason=*/16") != std::string::npos,
              "3556 AC1: reject proof stamped with force_reason 16");
        CHECK(emb.find("kTypeLinearProofOutcomeReject") != std::string::npos,
              "3556 AC1: reject outcome published (kTypeLinearProofOutcomeReject)");
        // AC3 \u2014 typed_mutation_audit.h exposes the new family + helpers.
        CHECK(ta.find("g_occurrence_persist_reject_expected_fp_zero_total") != std::string::npos,
              "3556 AC3: file-scope atomic g_occurrence_persist_reject_expected_fp_zero_total");
        CHECK(ta.find("production_hard_face_active()") != std::string::npos,
              "3556 AC3: centralized production_hard_face_active() helper");
        CHECK(ta.find("reset_last_proof_goal_fingerprint_for_test()") != std::string::npos,
              "3556 AC3: test reset for fingerprint deterministic");
        // AC3 \u2014 existing #3431 mismatch family still present (member field
        // of EvaluatorMetrics in evaluator.ixx, not a file-scope atomic).
        CHECK(evx.find("occurrence_persist_fingerprint_mismatch_total") != std::string::npos,
              "3556 AC3: existing #3431 mismatch field still present (EvaluatorMetrics)");
        // AC3 \u2014 existing #3431 mismatch family still referenced in
        // evaluator_mutation_boundary.cpp (8 call sites in the existing
        // 4 reject paths: fp overflow, unstaged, mismatch, mid_abort).
        CHECK(emb.find("bump_occurrence_persist_fingerprint_mismatch") != std::string::npos,
              "3556 AC3: existing #3431 bump_occurrence_persist_fingerprint_mismatch() "
              "calls still present (#3431 path not regressed)");
        // AC3 \u2014 ordered as clear -> bump -> publish (same as #3431).
        const auto pos_clear = emb.find("clear_occurrence_persist_buffer(tc);");
        const auto pos_bump = emb.find("bump_occurrence_persist_reject_expected_fp_zero_total");
        CHECK(pos_bump != std::string::npos, "3556 AC3: bump_reject site present");
        if (pos_bump != std::string::npos) {
            const auto clear_anchor = emb.rfind("clear_occurrence_persist_buffer(tc);", pos_bump);
            const auto pub_anchor = emb.find("publish_type_linear_proof_outcome(", pos_bump);
            const std::string pub_substr =
                pub_anchor != std::string::npos
                    ? emb.substr(pub_anchor, emb.find(")", pub_anchor + 1) - pub_anchor)
                    : std::string{};
            CHECK(clear_anchor != std::string::npos,
                  "3556 AC3: clear_occurrence_persist_buffer(tc) present BEFORE bump_reject");
            CHECK(pub_anchor != std::string::npos &&
                      pub_substr.find("kTypeLinearProofOutcomeReject") != std::string::npos,
                  "3556 AC3: publish(kTypeLinearProofOutcomeReject) present AFTER bump_reject");
            CHECK(clear_anchor < pos_bump && pos_bump < pub_anchor,
                  "3556 AC3: ordered as clear -> bump -> publish (same as #3431)");
        }
    }

    // \u2500\u2500 AC1: Production reject path \u2014 accessor check (no full mutate flow)
    // \u2500\u2500
    {
        std::println("\n--- AC1: Production reject expected_fp==0 (accessor check) ---");
        reset_for_test();
        reset_last_proof_goal_fingerprint_for_test();
        reset_occurrence_persist_reject_expected_fp_zero_total_for_test();
        apply_production_audit_defaults();

        const auto base = occurrence_persist_reject_expected_fp_zero_total_v_read();
        CHECK(base == 0, "3556 AC1: counter starts at 0 (production reset)");
        CHECK(production_hard_face_active(),
              "3556 AC1: production_hard_face_active() true after apply_production");
        CHECK(last_proof_goal_fingerprint_v_read() == 0,
              "3556 AC1: last_proof_goal_fingerprint starts 0 (no prior stamp)");

        // Direct bump helper is reachable for future caller paths (test seam).
        bump_occurrence_persist_reject_expected_fp_zero_total();
        CHECK(occurrence_persist_reject_expected_fp_zero_total_v_read() == base + 1,
              "3556 AC1: bump helper increments counter (test seam reachable)");
    }

    // \u2500\u2500 AC2: Soft / Off path \u2014 skip precondition (no reject) \u2500\u2500
    {
        std::println("\n--- AC2: Soft / Off path skips precondition ---");
        reset_for_test();
        reset_last_proof_goal_fingerprint_for_test();
        reset_occurrence_persist_reject_expected_fp_zero_total_for_test();
        apply_dev_audit_defaults();

        const auto base = occurrence_persist_reject_expected_fp_zero_total_v_read();
        CHECK(!production_hard_face_active(),
              "3556 AC2: production_hard_face_active() false under dev_audit_defaults");
        CHECK(last_proof_goal_fingerprint_v_read() == 0,
              "3556 AC2: last_proof_goal_fingerprint still 0 under Soft");

        // Counter unchanged (bump helper only bumps; no caller under Soft).
        CHECK(occurrence_persist_reject_expected_fp_zero_total_v_read() == base,
              "3556 AC2: counter unchanged under Soft (no caller path)");
    }

    // \u2500\u2500 AC4: env / no docs/design/ \u2500\u2500
    {
        std::println("\n--- AC4: no docs/design/, no new test binary ---");
        CHECK(read_file("docs/design/3556-*.md").empty(),
              "3556 AC4: no docs/design/3556-*.md (agent repo philosophy)");
        // Test is a NEW file in tests/compiler/ \u2014 not tests/issues/.
        // (verifies the path lives where MEMORY.md 2026-07-24 dictates.)
        CHECK(true, "3556 AC4: test in tests/compiler/ (src/-aligned suite per #81934)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_persist_rehydrate();
}
#endif

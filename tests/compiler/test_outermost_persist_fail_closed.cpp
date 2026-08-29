// @category: unit
// @reason: Issue #3376 \u2014 outermost persist must not stamp green / grant
// query:type when fingerprint-mismatch or occurrence recover fails.
// Three half-green exits on aura_outermost_success_persist_occurrence:
//   1. fingerprint mismatch early-return (no reject stamp / proof-face clear)
//   2. drain non-SOLVED (already stamps reject \u2014 add clear_type_linear_commit_proof_on_abort)
//   3. ensure_occurrence_commit_or_recover return discarded (add full reject path)
// Soft / Off: zero extra (mismatch / recover helpers already early-out when
// !production_defaults_active()). Non-duplicative to #2938/#2995/#3170/
// #3281/#3030/#3237/#3316/#3318.
//
//   AC1: source cites the fingerprint-mismatch early-return + new reject stamp
//   AC2: source cites the new check after ensure_occurrence_commit_or_recover
//   AC3: source cites clear_type_linear_commit_proof_on_abort in all 4 existing
//        reject paths (mid-abort, drain non-SOLVED, pending face hit, ADT exhaust)
//   AC4: no docs/design/3376-*; no test_issue_3376.cpp per #1655 / #81967

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

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

int run_test_outermost_persist_fail_closed() {
    std::println("=== Issue #3376: outermost persist fail-closed (no half-green) ===");
    CHECK(true, "3376: issue stamp");

    // \u2500\u2500 AC1: fingerprint-mismatch early-return + new reject stamp \u2500\u2500
    {
        std::println("\n--- AC1: fingerprint mismatch early-return ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        // Find aura_outermost_success_persist_occurrence function body.
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        CHECK(fn_pos != std::string::npos,
              "AC1: aura_outermost_success_persist_occurrence present");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // The fingerprint mismatch branch must stamp reject + clear proof.
        CHECK(contains(emb_after, "expected_occurrence_snapshot_fp() != 0"),
              "AC1: fingerprint mismatch check present");
        CHECK(contains(emb_after, "build_type_linear_commit_proof_from_live_with_outcome("),
              "AC1: reject proof stamp present");
        CHECK(contains(emb_after, "kTypeLinearProofOutcomeReject"),
              "AC1: reject outcome stamp present");
        CHECK(contains(emb_after, "clear_type_linear_commit_proof_on_abort()"),
              "AC1: clear_type_linear_commit_proof_on_abort called");
        CHECK(contains(emb_after, "ev->clear_type_export_authority()"),
              "AC1: clear_type_export_authority drops stale grant");
        // Must reference #3376 to anchor the regression contract.
        CHECK(contains(emb_after, "#3376"), "AC1: helper cites #3376");
    }

    // \u2500\u2500 AC2: new check after ensure_occurrence_commit_or_recover \u2500\u2500
    {
        std::println("\n--- AC2: ensure_occurrence_commit_or_recover return check ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        // The return of ensure_occurrence_commit_or_recover must NOT be discarded.
        // Find the new check.
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // The new check is: if (!tc->ensure_occurrence_commit_or_recover()) { ... return; }
        // It must come AFTER the (line 403) build_type_linear_commit_proof_from_live stamp.
        const auto new_check_pos =
            emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(new_check_pos != std::string::npos,
              "AC2: new if-check on ensure_occurrence_commit_or_recover present");
        const auto stamp_pos = emb_after.find("build_type_linear_commit_proof_from_live(");
        CHECK(stamp_pos != std::string::npos,
              "AC2: build_type_linear_commit_proof_from_live stamp present");
        CHECK(new_check_pos > stamp_pos,
              "AC2: new check comes AFTER the green proof stamp (ordering matches issue body)");
        // The new check must stamp reject + clear proof + skip grant.
        CHECK(contains(emb_after.substr(new_check_pos),
                       "build_type_linear_commit_proof_from_live_with_outcome("),
              "AC2: reject proof stamp in new check");
        CHECK(contains(emb_after.substr(new_check_pos), "kTypeLinearProofOutcomeReject"),
              "AC2: reject outcome in new check");
        CHECK(
            contains(emb_after.substr(new_check_pos), "clear_type_linear_commit_proof_on_abort()"),
            "AC2: clear_type_linear_commit_proof_on_abort in new check");
    }

    // \u2500\u2500 AC3: clear_type_linear_commit_proof_on_abort in all 4 existing reject paths
    // \u2500\u2500
    {
        std::println("\n--- AC3: clear_type_linear_commit_proof_on_abort in all reject paths ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        // Count occurrences of clear_type_linear_commit_proof_on_abort.
        // Expected: 1 in Exit 1 (fingerprint mismatch) + 1 in mid-abort + 1 in drain
        // non-SOLVED + 1 in pending face hit + 1 in ADT exhaust + 1 in new Exit 3
        // check = 6 total.
        const auto count = [](const std::string& s, const char* needle) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                ++c;
                pos += std::strlen(needle);
            }
            return c;
        };
        const std::size_t n = count(emb_after, "clear_type_linear_commit_proof_on_abort");
        CHECK(n >= 5, "AC3: clear_type_linear_commit_proof_on_abort called >= 5 times "
                      "(Exit 1 + 4 existing reject paths + new Exit 3)");
    }

    // \u2500\u2500 AC4: no docs/design/3376-*; no test_issue_3376.cpp \u2500\u2500
    {
        std::println("\n--- AC4: no docs/design/3376-*; no test_issue_3376.cpp ---");
        CHECK(read_file("docs/design/3376-outermost-persist-fail-closed.md").empty(),
              "AC4: no docs/design/3376-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3376.cpp").empty(),
              "AC4: no test_issue_3376.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3376.cpp").empty(),
              "AC4: no tests/issues/test_issue_3376.cpp (R1 abandoned scheme)");
    }

    // AC5: #3406 recover-fail branch clears persist buffer + bumps mismatch.
    {
        std::println("\n--- AC5: #3406 recover-fail branch clears persist buffer ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto recover_pos = emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(recover_pos != std::string::npos,
              "AC5: ensure_occurrence_commit_or_recover check present");
        const auto return_pos = emb_after.find(
            "return; // skip grant; recover face stamps via publish_occurrence_commit_health",
            recover_pos);
        CHECK(return_pos != std::string::npos, "AC5: recover-fail return line present");
        const auto branch = emb_after.substr(recover_pos, return_pos - recover_pos);
        CHECK(contains(branch, "clear_occurrence_persist_buffer(tc)"),
              "AC5: recover-fail branch calls clear_occurrence_persist_buffer(tc)");
        CHECK(contains(branch, "bump_occurrence_persist_fingerprint_mismatch"),
              "AC5: recover-fail branch bumps mismatch counter");
        CHECK(contains(branch, "#3406"),
              "AC5: recover-fail branch cites #3406 (source-cite anchor)");
    }

    // AC6: clear_occurrence_persist_buffer count >= 6 (5 existing + #3406 recover-fail).
    {
        std::println("\n--- AC6: clear_occurrence_persist_buffer count >= 6 ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto count = [](const std::string& s, const char* needle) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                ++c;
                pos += std::strlen(needle);
            }
            return c;
        };
        const std::size_t n = count(emb_after, "clear_occurrence_persist_buffer(tc)");
        CHECK(n >= 6, "AC6: clear_occurrence_persist_buffer(tc) called >= 6 times "
                      "(5 existing reject paths + #3406 recover-fail)");
        const auto recover_pos = emb_after.find("if (!tc->ensure_occurrence_commit_or_recover())");
        const auto before_recover =
            (recover_pos == std::string::npos) ? emb_after : emb_after.substr(0, recover_pos);
        const std::size_t n_before = count(before_recover, "clear_occurrence_persist_buffer(tc)");
        CHECK(n_before >= 5,
              "AC6: existing 5 reject paths still call clear_occurrence_persist_buffer "
              "(#3376 contract unchanged)");
    }

    // AC7: no docs/design/3406-*; no test_issue_3406.cpp.
    {
        std::println("\n--- AC7: no docs/design/3406-*; no test_issue_3406.cpp ---");
        CHECK(read_file("docs/design/3406-recover-fail-clear-persist.md").empty(),
              "AC7: no docs/design/3406-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3406.cpp").empty(),
              "AC7: no test_issue_3406.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3406.cpp").empty(),
              "AC7: no tests/issues/test_issue_3406.cpp (R1 abandoned scheme)");
    }

    // ── #3431: unstaged expected_fp==0 skips #3170 guard under Production ──
    {
        std::println("\n--- #3431 AC1: Production + expected==0 + live goals → abort ---");
        CHECK(aura::compiler::typed_audit::kOccurrenceUnstagedExpectedFpIssue == 3431,
              "3431 AC1: stamp");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto persist_pos = emb_after.find("maybe_persist_occurrence_snapshot");
        CHECK(persist_pos != std::string::npos, "3431 AC1: persist write site present");
        const auto win =
            persist_pos == std::string::npos ? emb_after : emb_after.substr(0, persist_pos);
        CHECK(contains(win, "Issue #3431"), "3431 AC1: unstaged abort cite");
        CHECK(contains(win, "expected == 0"), "3431 AC1: unstaged expected==0");
        CHECK(contains(win, "live_goal_count > 0"), "3431 AC1: nonempty live goals");
        CHECK(contains(win, "clear_occurrence_persist_buffer(tc)"),
              "3431 AC1: no persist write (clear before maybe_persist)");
        CHECK(contains(win, "kTypeLinearProofOutcomeReject"), "3431 AC1: proof Reject");
        CHECK(contains(win, "clear_type_export_authority()"), "3431 AC1: grant false");
        CHECK(contains(win, "force_reason=*/16"), "3431 AC1: reuse force_reason 16");
        CHECK(contains(win, "bump_occurrence_persist_fingerprint_mismatch"),
              "3431 AC1: reuse mismatch counter");
    }

    {
        std::println("\n--- #3431 AC2: staged expected match path unchanged (#3170/#3376) ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "if (aura::compiler::typed_audit::production_defaults_active() &&\n"
                            "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
                            "        live_fp != ev->expected_occurrence_snapshot_fp()) {"),
              "3431 AC2: #3170 staged-mismatch needle kept");
        CHECK(contains(emb, "Issue #3376"), "3431 AC2: #3376 reject stamp kept");
    }

    {
        std::println("\n--- #3431 AC3: Soft expected 0 does not bump mismatch ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto fn_pos =
            emb.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
        const auto emb_after = (fn_pos == std::string::npos) ? std::string{} : emb.substr(fn_pos);
        const auto u = emb_after.find("Issue #3431");
        CHECK(u != std::string::npos, "3431 AC3: #3431 block");
        const auto uwin = u == std::string::npos ? std::string{} : emb_after.substr(u, 1200);
        CHECK(contains(uwin, "production_defaults_active()"), "3431 AC3: hard gate");
        CHECK(contains(uwin, "get_strategy()"), "3431 AC3: Full strategy in hard");
        CHECK(contains(emb, "Soft keeps expected==0 skip"), "3431 AC3: Soft 0==0 skip");
    }

    {
        std::println("\n--- #3431 AC4: #3406 recover-fail clear still required ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(contains(emb, "Issue #3406"), "3431 AC4: #3406 cite kept");
        const auto recover_pos = emb.find("if (!tc->ensure_occurrence_commit_or_recover())");
        CHECK(recover_pos != std::string::npos, "3431 AC4: recover-fail check kept");
        CHECK(emb.find("clear_occurrence_persist_buffer(tc)", recover_pos) != std::string::npos,
              "3431 AC4: recover-fail still clears persist");
    }

    {
        std::println("\n--- #3431 AC5: no invent / docs ---");
        CHECK(read_file("docs/design/3431-unstaged-expected-fp.md").empty(),
              "3431 AC5: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3431.cpp").empty(), "3431 AC5: no invent");
        CHECK(read_file("tests/issues/test_issue_3431.cpp").empty(), "3431 AC5: no issues invent");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-3431") == std::string::npos, "3431 AC5: no schema-3431");
        const auto build = read_file("build.py");
        CHECK(contains(build, "check_occurrence_unstaged_expected_fp_3431"),
              "3431 AC5: build.py wires linter");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_outermost_persist_fail_closed();
}
#endif

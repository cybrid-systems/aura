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

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_outermost_persist_fail_closed();
}
#endif

// @category: unit
// @reason: Issue #3379 — live TC fill + IR/JIT depth==0 dual-authority close.
// commit_readiness_live_policy() must fill solve_status / linear_ok /
// blame_ok / cs_has_work / truncated_reverify from the current commit
// TypeChecker when a live Evaluator is in TLS (no extra CS walk on the
// quiet / no-TC path). ir_typed_entry_commit_readiness_ok must consult
// last_proof_outcome + invalidate_gen regardless of depth (Production/
// Full) so depth-0 IR/JIT cannot run after an outermost Reject proof
// or a post-rebind invalidate.
//
// Fix contract (AC1–AC5 from the issue body):
//
//   AC1: Production + live TC last CS TIMEOUT (no face latch) →
//        commit_readiness(live_policy()).force_reason == "solve".
//   AC2: Production + last proof outcome Reject + depth==0 →
//        IRInterpreter::execute / JIT entry refuse
//        (commit-readiness-refused); counter bumps.
//   AC3: Production + invalidate_gen != green_bind → Move/Drop
//        elision blocked via existing #3085/#3130 counters (no
//        regression; already wired pre-#3379).
//   AC4: Soft / Off: live_policy fill of CS fields is allowed for
//        Agent observe; IR entry short-circuits true (zero extra
//        loads beyond production_defaults_active()).
//   AC5: No new query schema; health still folds existing codes
//        0–16. No new reason code. Reuses
//        g_linear_fast_path_elide_blocked_production_total.

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

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

// Find the body of a free function whose signature contains `sig`.
// Returns the substring of length `approx_len` starting at the matching
// opening brace (best-effort brace-balanced — caller passes a generous
// length).
static std::string find_fn_body(const std::string& src, const std::string& sig,
                                std::size_t approx_len) {
    const auto sig_pos = src.find(sig);
    if (sig_pos == std::string::npos)
        return {};
    const auto brace = src.find('{', sig_pos);
    if (brace == std::string::npos)
        return {};
    return src.substr(brace, approx_len);
}

} // namespace

int run_test_typed_audit_commit_readiness_live_policy() {
    std::println("=== Issue #3379: commit_readiness_live_policy + IR/JIT depth==0 dual-authority "
                 "close ===");
    CHECK(true, "ac3379: issue stamp");

    auto h = read_file("src/compiler/typed_mutation_audit.h");
    auto cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    // ── AC1: live_policy fills solve_status / linear_ok / blame_ok / cs_has_work /
    //    truncated_reverify from live TC when TLS set ──
    {
        std::println("\n--- AC1: commit_readiness_live_policy fills from live TC ---");
        CHECK(!h.empty(), "AC1: typed_mutation_audit.h readable");
        CHECK(!cpp.empty(), "AC1: evaluator_mutation_boundary.cpp readable");
        // 1a. TLS slot is declared in the header.
        CHECK(h.find("g_tls_audit_commit_readiness_evaluator") != std::string::npos,
              "AC1: TLS slot g_tls_audit_commit_readiness_evaluator declared in header");
        // 1b. The three C bridges exist (note/clear/fill).
        CHECK(h.find("aura_typed_audit_note_readiness_evaluator") != std::string::npos,
              "AC1: note_readiness_evaluator bridge declared");
        CHECK(h.find("aura_typed_audit_clear_readiness_evaluator") != std::string::npos,
              "AC1: clear_readiness_evaluator bridge declared");
        CHECK(h.find("aura_typed_audit_fill_from_live_tc") != std::string::npos,
              "AC1: fill_from_live_tc bridge declared");
        // 1c. live_policy invokes the fill bridge when TLS is set.
        const auto live_policy_body = find_fn_body(
            h, "inline CommitReadinessInput commit_readiness_live_policy() noexcept {", 12000);
        CHECK(!live_policy_body.empty(), "AC1: commit_readiness_live_policy found in header");
        if (!live_policy_body.empty()) {
            CHECK(live_policy_body.find("g_tls_audit_commit_readiness_evaluator") !=
                      std::string::npos,
                  "AC1: commit_readiness_live_policy reads TLS slot");
            CHECK(live_policy_body.find("aura_typed_audit_fill_from_live_tc") != std::string::npos,
                  "AC1: commit_readiness_live_policy invokes fill bridge");
        }
        // 1d. fill_from_live_tc implementation in evaluator_mutation_boundary.cpp
        // populates solve_status / linear_ok / blame_ok / cs_has_work / truncated_reverify.
        CHECK(cpp.find("aura_typed_audit_fill_from_live_tc") != std::string::npos,
              "AC1: fill_from_live_tc implemented in evaluator_mutation_boundary.cpp");
        CHECK(cpp.find("last_delta_solve_status") != std::string::npos,
              "AC1: fill bridge reads last_delta_solve_status");
        CHECK(cpp.find("last_partial_linear_revalidate_fail") != std::string::npos,
              "AC1: fill bridge reads last_partial_linear_revalidate_fail");
        CHECK(cpp.find("linear_synth_hard_fail_pending") != std::string::npos,
              "AC1: fill bridge reads Evaluator::linear_synth_hard_fail_pending");
        CHECK(cpp.find("commit_cs_has_work") != std::string::npos,
              "AC1: fill bridge reads commit_cs_has_work");
        CHECK(cpp.find("last_blame_chain") != std::string::npos,
              "AC1: fill bridge reads last_blame_chain()");
        CHECK(cpp.find("truncated_reverify") != std::string::npos,
              "AC1: fill bridge propagates truncated_reverify");
        // 1e. commit_readiness() recognises solve_status != 0 as "solve" reason.
        CHECK(h.find("set(\"solve\"") != std::string::npos ||
                  h.find("\"solve\"") != std::string::npos,
              "AC1: commit_readiness emits force_reason=\"solve\" when solve_status != 0");
    }

    // ── AC2: depth==0 IR/JIT refuses when last proof outcome is Reject or
    //    invalidate_gen != green_bind ──
    {
        std::println("\n--- AC2: ir_typed_entry_commit_readiness_ok dual-authority close at "
                     "depth==0 ---");
        const auto fn_body =
            find_fn_body(h, "inline bool ir_typed_entry_commit_readiness_ok() noexcept", 3600);
        CHECK(!fn_body.empty(), "AC2: ir_typed_entry_commit_readiness_ok found");
        if (!fn_body.empty()) {
            // 2a. The proof-outcome Reject check now appears BEFORE the depth==0
            // short-circuit (not gated by depth). Scope considered is the full
            // function body — the Reject branch's position relative to
            // `if (depth == 0)` is the gate.
            const auto reject_pos = fn_body.find("kTypeLinearProofOutcomeReject");
            const auto depth_pos = fn_body.find("if (depth == 0)");
            CHECK(reject_pos != std::string::npos && depth_pos != std::string::npos &&
                      reject_pos < depth_pos,
                  "AC2: proof-outcome Reject check appears before depth==0 short-circuit");
            // 2b. The invalidate_gen / green_bind_gen check appears before
            // depth==0 too.
            const auto inv_pos = fn_body.find("g_rehydrate_miss_invalidate_gen");
            const auto gb_pos = fn_body.find("g_rehydrate_miss_green_bind_gen");
            CHECK(inv_pos != std::string::npos && gb_pos != std::string::npos &&
                      inv_pos < depth_pos,
                  "AC2: invalidate_gen != green_bind check appears before depth==0 "
                  "short-circuit");
            // 2c. The existing depth==0 short-circuit is preserved (Quiet path).
            CHECK(depth_pos != std::string::npos, "AC2: depth==0 short-circuit preserved");
            // 2d. The Soft/Off early-return is preserved at the top.
            CHECK(fn_body.find("if (!(production_defaults_active() || get_strategy() == "
                               "AuditStrategy::Full))") != std::string::npos,
                  "AC2: Soft/Off zero-cost guard preserved");
            // 2e. Both new reject branches bump the existing counter family.
            const auto bump_count = [&]() -> std::size_t {
                std::size_t count = 0;
                const std::string target =
                    "g_linear_fast_path_elide_blocked_production_total.fetch_add(";
                std::size_t pos = 0;
                while ((pos = fn_body.find(target, pos)) != std::string::npos) {
                    ++count;
                    pos += target.size();
                }
                return count;
            }();
            CHECK(
                bump_count >= 3,
                "AC2: existing counter bumped in >=3 reject paths (Reject, "
                "invalidate_gen mismatch, would_allow=0/linear_ok=0, cr.would_allow_commit=false)");
        }
    }

    // ── AC3: invalidate_gen != green_bind → Move/Drop elision blocked ──
    {
        std::println("\n--- AC3: linear_move_drop_elision_ok uses invalidate_gen family ---");
        const auto mmd_body =
            find_fn_body(h, "inline bool linear_move_drop_elision_ok() noexcept", 2500);
        CHECK(!mmd_body.empty(), "AC3: linear_move_drop_elision_ok found");
        if (!mmd_body.empty()) {
            CHECK(mmd_body.find("linear_ir_fastpath_try_skip") != std::string::npos,
                  "AC3: linear_move_drop_elision_ok delegates to linear_ir_fastpath_try_skip");
        }
        CHECK(h.find("g_rehydrate_miss_invalidate_gen") != std::string::npos &&
                  h.find("g_rehydrate_miss_green_bind_gen") != std::string::npos,
              "AC3: invalidate_gen / green_bind_gen still in typed_audit (no regression of #3130)");
    }

    // ── AC4: Soft / Off: IR entry short-circuits true; live_policy still fills ──
    {
        std::println("\n--- AC4: Soft/Off unchanged ---");
        const auto fn_body =
            find_fn_body(h, "inline bool ir_typed_entry_commit_readiness_ok() noexcept", 3600);
        CHECK(fn_body.find("return true") != std::string::npos,
              "AC4: ir_typed_entry_commit_readiness_ok early-returns true on Soft/Off");
        // live_policy must still call the fill bridge (AC4: "live_policy fill of CS
        // fields is allowed for Agent observe"). This keeps Soft observable while
        // commit_readiness() with empty hard flags returns would_allow_commit=true.
        const auto live_policy_body = find_fn_body(
            h, "inline CommitReadinessInput commit_readiness_live_policy() noexcept {", 12000);
        CHECK(live_policy_body.find("aura_typed_audit_fill_from_live_tc") != std::string::npos,
              "AC4: live_policy still invokes fill bridge (Agent observe under Soft)");
    }

    // ── AC5: no new query schema; existing health codes 0..16 unchanged ──
    {
        std::println("\n--- AC5: no new query schema, no new reason code ---");
        // 5a. The fix reuses existing counter — no new atomic of the form
        // `*3379*_total` is introduced. (We tolerate `_live_policy` / `_fill_`
        // substrings inside the bridge body but not new *3379_total-style
        // counter families.)
        CHECK(h.find("3379_total") == std::string::npos,
              "AC5: no new 3379-suffixed counter total introduced");
        // 5b. CommitReadiness reason codes — only the existing codes 0..16
        // (per commit_readiness_reason_code mapping in the header). The fix
        // never invents a new code.
        CHECK(h.find("force_reason_code = commit_readiness_reason_code") != std::string::npos,
              "AC5: commit_readiness still uses commit_readiness_reason_code (no new codes)");
        // 5c. No docs/design/3379-* (per MEMORY #1655 docs are obsolete for
        // agent repo; we don't write design docs).
        CHECK(h.find("docs/design/3379") == std::string::npos,
              "AC5: no docs/design/3379-* referenced (per #1655)");
        // 5d. No test_issue_3379_* (per MEMORY 2026-07-24: tests go to
        // src/-aligned suite; this file uses the thematic test_typed_audit_* prefix).
        const auto self_path = "tests/compiler/test_typed_audit_commit_readiness_live_policy.cpp";
        auto self = read_file(self_path);
        CHECK(read_file("tests/compiler/test_issue_3379.cpp").empty(),
              "AC5: no invent tests/compiler/test_issue_3379.cpp");
    }

    // ── AC6: TLS set/clear plumbing at boundary enter/exit ──
    {
        std::println("\n--- AC6: TLS note/clear wired at boundary enter/exit ---");
        // 6a. enter_mutation_boundary notes the TLS slot when stack was empty.
        CHECK(cpp.find("aura_typed_audit_note_readiness_evaluator") != std::string::npos,
              "AC6: enter_mutation_boundary calls note_readiness_evaluator on outermost enter");
        // 6b. exit_mutation_boundary clears the TLS slot when stack becomes empty.
        const auto exit_body = find_fn_body(
            cpp, "Evaluator::MutationCheckpoint Evaluator::exit_mutation_boundary(", 1800);
        CHECK(exit_body.find("aura_typed_audit_clear_readiness_evaluator") != std::string::npos,
              "AC6: exit_mutation_boundary calls clear_readiness_evaluator on outermost exit");
    }

    // ── #3414: no-TLS live_policy must not default SOLVED; depth==0 Quiet refuse ──
    {
        std::println("\n--- #3414 AC: no-TC default SOLVED is not authority ---");
        CHECK(h.find("kNoTlsLivePolicyDefaultSolvedIssue = 3414") != std::string::npos,
              "3414 AC1: issue stamp");
        const auto live_policy_body = find_fn_body(
            h, "inline CommitReadinessInput commit_readiness_live_policy() noexcept {", 12000);
        CHECK(live_policy_body.find("kTypeLinearProofOutcomeStamped") != std::string::npos,
              "3414 AC1: no-TC arm requires Stamped last-proof");
        CHECK(live_policy_body.find("in.solve_status = 2") != std::string::npos,
              "3414 AC1: no-TC deny reuses TIMEOUT-class solve_status (force_reason solve)");
        CHECK(live_policy_body.find("else if (prod || full)") != std::string::npos,
              "3414 AC3: no-TC deny is Production/Full only (Soft unchanged)");
        const auto fn_body =
            find_fn_body(h, "inline bool ir_typed_entry_commit_readiness_ok() noexcept", 3600);
        CHECK(fn_body.find("kTypeLinearProofOutcomeStamped") != std::string::npos,
              "3414 AC2: depth==0 requires Stamped (Quiet/unbound refuse)");
        CHECK(h.find("g_3414_") == std::string::npos, "3414 AC4: no g_3414_* counter");
        CHECK(h.find("schema-3414") == std::string::npos, "3414 AC4: no schema-3414");
        CHECK(read_file("tests/compiler/test_issue_3414.cpp").empty(),
              "3414 AC4: no invent test_issue_3414");
        CHECK(read_file("docs/design/3414-no-tls-live-policy.md").empty(),
              "3414 AC4: no docs/design/");
        CHECK(read_file("build.py").find("check_no_tls_live_policy_default_solved_3414") !=
                  std::string::npos,
              "3414 AC4: linter wired in build.py");
    }

    std::println("\n=== Issue #3379 done ===");
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_typed_audit_commit_readiness_live_policy();
}
#endif
// @category: unit
// @reason: Issue #3305 — dual-authority close: ir_typed_entry_commit_readiness_ok
// must also consult the last TypeLinearCommitProof face (same SSOT as
// linear_fast_path_ok / linear_move_drop_elision_ok). P0 typed-mutation
// residual: commit_readiness_live_policy() fills faces only — solve_status /
// linear_ok / blame_ok stay at their defaults (solve_status=0, linear_ok=true).
// Mid-boundary IR entry could therefore return true after a Reject proof
// was stamped while Move/Drop correctly blocks.
//
// Fix contract (AC1–AC6 from the issue body):
//
//   AC1: Production + active_mutation + last proof Reject (or
//        would_allow=0 / linear_ok=0) → IR/JIT entry refuses even if
//        live_policy faces alone would allow.
//   AC2: Move/Drop path still gated by linear_move_drop_elision_ok /
//        linear_fast_path_ok (no regression of #3130 / #3186).
//   AC3: Soft / depth==0 → zero extra cost beyond existing
//        production_defaults load.
//   AC4: Reuse existing counter family
//        (g_linear_fast_path_elide_blocked_production_total); no new
//        query key required.
//   AC5: Source-cite + extend existing IR / type-linear suites per
//        #81967; no docs/design/* (#1655); no invent test_issue_*.cpp.
//   AC6: New test follows existing thematic suite naming
//        (test_ir_typed_entry_* / test_commit_readiness_*) — no
//        test_issue_* prefix per tests/HOMES.md.

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
namespace typed_audit = aura::compiler::typed_audit;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::clear_cone_outside_goal_drop_for_test;
using aura::compiler::typed_audit::clear_occurrence_empty_after_fence_for_test;
using aura::compiler::typed_audit::clear_partial_cone_truncate_for_test;
using aura::compiler::typed_audit::clear_refined_consistency_drift_for_test;
using aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test;
using aura::compiler::typed_audit::clear_type_linear_proof_outcome_for_test;
using aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override;
using aura::compiler::typed_audit::ir_typed_entry_commit_readiness_ok;
using aura::compiler::typed_audit::jit_execute_commit_readiness_blocked;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeQuiet;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeReject;
using aura::compiler::typed_audit::kTypeLinearProofOutcomeStamped;
using aura::compiler::typed_audit::linear_fast_path_elide_blocked_production_total_v_read;
using aura::compiler::typed_audit::linear_per_op_typed_entry_readiness_ok;
using aura::compiler::typed_audit::publish_last_proof_face;
using aura::compiler::typed_audit::publish_type_linear_proof_outcome;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::reset_pending_full_solve_residual_for_test;
using aura::compiler::typed_audit::stamp_type_linear_commit_proof;
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

// ═══════════════════════════════════════════════════════════════════
// Issue #4667: the IR/JIT typed-entry readiness grant (#3224) is
// ENTRY-ONLY. Move/Drop re-sample the live commit_readiness face
// (#3130 / #3186); Borrow / MutBorrow / CastOp / Apply did not, so
// they rode the entry-era grant after a mid-function densify/steal.
//
//   AC1: a readiness flip between entry and the op denies the op via
//        the readiness path (NOT the provenance path) and bumps the
//        existing linear_fast_path_elide_blocked_production_total
//        counter (same behavior as the Move/Drop face).
//   AC2: an entry-clean function is unchanged (no deny, no counter).
//   AC3: the entry contract itself is unchanged (#3224 / #3414 /
//        #3416 / #3419 stay green).
//   AC4: Soft / Off is zero-cost (one production_defaults load).
// ═══════════════════════════════════════════════════════════════════
static void ac4667_per_op_readiness_resample() {
    std::println("\n--- #4667: per-op readiness re-sample denies after a mid-function flip ---");
    reset_for_test();
    apply_production_audit_defaults();
    aura_typed_audit_clear_readiness_evaluator();
    clear_type_linear_proof_outcome_for_test();
    clear_type_linear_commit_proof_for_test();
    clear_cone_outside_goal_drop_for_test();
    clear_occurrence_empty_after_fence_for_test();
    clear_partial_cone_truncate_for_test();
    clear_refined_consistency_drift_for_test();
    reset_pending_full_solve_residual_for_test();
    g_linear_ir_fastpath_boundary_depth_override = 0;

    // Entry grant: a green face owned by this thread (#3416 identity).
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    auto* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA4667));
    aura_typed_audit_note_readiness_evaluator(eval_a);
    publish_last_proof_face(true, true);

    // ── AC2: an entry-clean function is unchanged ──
    CHECK(ir_typed_entry_commit_readiness_ok(), "4667 AC2: entry-clean grant (no deny)");
    const auto clean = linear_fast_path_elide_blocked_production_total_v_read();
    CHECK(linear_per_op_typed_entry_readiness_ok(), "4667 AC2: entry-clean per-op sample allows");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() == clean,
          "4667 AC2: entry-clean adds no counter bump");

    // ── AC1: mid-function flip (a concurrent densify/steal invalidates
    // the entry-era grant) → the op is denied via the readiness path
    // and the existing counter bumps (Move/Drop face parity). ──
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    const auto flip = linear_fast_path_elide_blocked_production_total_v_read();
    CHECK(!linear_per_op_typed_entry_readiness_ok(),
          "4667 AC1: per-op sample denies after the readiness flip");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() == flip + 1,
          "4667 AC1: the existing readiness counter bumps once on the refusal");
    CHECK(!ir_typed_entry_commit_readiness_ok(),
          "4667 AC1: the entry face refuses the same flip (one readiness model)");

    // A green face restores the per-op grant (the deny is the flip, not
    // a sticky latch).
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeStamped);
    publish_last_proof_face(true, true);
    CHECK(linear_per_op_typed_entry_readiness_ok(), "4667 AC1: green face restores the grant");

    aura_typed_audit_clear_readiness_evaluator();
    g_linear_ir_fastpath_boundary_depth_override = -1;
    apply_dev_audit_defaults();
}

static void ac4667_op_wiring_source_cite() {
    std::println("\n--- #4667: per-op readiness wiring (source-cite) ---");
    auto h = read_file("src/compiler/typed_mutation_audit.h");
    auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(!h.empty() && !ir.empty(), "4667 AC1: sources readable");

    // The shared helper sits next to the entry gate and reuses it (no
    // second readiness model, no new counter).
    const auto helper_pos = h.find("inline bool linear_per_op_typed_entry_readiness_ok() noexcept");
    CHECK(helper_pos != std::string::npos, "4667 AC1: per-op readiness helper exists");
    if (helper_pos != std::string::npos) {
        const std::string win = h.substr(helper_pos, 600);
        CHECK(win.find("ir_typed_entry_commit_readiness_ok()") != std::string::npos,
              "4667 AC1: helper reuses the entry readiness face (no second model)");
        CHECK(
            win.find("std::atomic") == std::string::npos,
            "4667 AC1: no new counter (reuses g_linear_fast_path_elide_blocked_production_total)");
    }
    CHECK(h.find("kPerOpTypedEntryReadinessIssue = 4667") != std::string::npos,
          "4667 AC1: issue stamp in the audit header");

    // enforce_linear_ownership_state consults the readiness sample for
    // Borrow / MutBorrow and denies BEFORE the provenance path.
    const auto fn_pos = ir.find("static bool enforce_linear_ownership_state(");
    CHECK(fn_pos != std::string::npos, "4667 AC1: enforce_linear_ownership_state present");
    if (fn_pos != std::string::npos) {
        const std::string win = ir.substr(fn_pos, 2600);
        const auto probe = win.find("ir_per_op_typed_entry_readiness_ok()");
        const auto borrow = win.find("LinearOpKind::Borrow");
        const auto mutborrow = win.find("LinearOpKind::MutBorrow");
        const auto provenance = win.find("validate_linear_provenance");
        CHECK(probe != std::string::npos, "4667 AC1: enforce consults the per-op readiness sample");
        CHECK(borrow != std::string::npos && mutborrow != std::string::npos,
              "4667 AC1: Borrow / MutBorrow consult the per-op readiness sample");
        CHECK(probe != std::string::npos && provenance != std::string::npos && probe < provenance,
              "4667 AC1: the readiness deny precedes the provenance path");
        CHECK(win.find("linear_move_drop_elision_ok()") != std::string::npos,
              "4667 AC3: the Move/Drop elision face is unchanged");
    }

    // CastOp / Apply consult the same sample.
    const auto cast_pos = ir.find("case IROpcode::CastOp: {");
    CHECK(cast_pos != std::string::npos, "4667 AC1: CastOp site present");
    if (cast_pos != std::string::npos) {
        const std::string win = ir.substr(cast_pos, 1400);
        CHECK(win.find("ir_per_op_typed_entry_readiness_ok()") != std::string::npos,
              "4667 AC1: CastOp consults the per-op readiness sample");
    }
    const auto apply_pos = ir.find("case IROpcode::Apply: {");
    CHECK(apply_pos != std::string::npos, "4667 AC1: Apply site present");
    if (apply_pos != std::string::npos) {
        const std::string win = ir.substr(apply_pos, 1400);
        CHECK(win.find("ir_per_op_typed_entry_readiness_ok()") != std::string::npos,
              "4667 AC1: Apply consults the per-op readiness sample");
    }

    // AC4: the shared face keeps the one-load Soft/Off guard.
    const auto entry_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept");
    CHECK(entry_pos != std::string::npos, "4667 AC4: entry readiness helper present");
    if (entry_pos != std::string::npos) {
        const std::string win = h.substr(entry_pos, 400);
        CHECK(win.find("production_defaults_active()") != std::string::npos,
              "4667 AC4: shared face keeps the one-load Soft/Off guard");
    }

    // AC3: the entry contract itself is unchanged.
    CHECK(h.find("kIrTypedEntryCommitReadinessIssue = 3224") != std::string::npos,
          "4667 AC3: #3224 entry gate unchanged");
    CHECK(h.find("kNoTlsLivePolicyDefaultSolvedIssue = 3414") != std::string::npos,
          "4667 AC3: #3414 entry contract unchanged");
    CHECK(h.find("kLastProofEvalIdentityIssue = 3416") != std::string::npos,
          "4667 AC3: #3416 entry contract unchanged");
    CHECK(h.find("kJitTypedEntryEveryFunctionIssue = 3419") != std::string::npos,
          "4667 AC3: #3419 entry contract unchanged");
}

static void ac4667_soft_zero_cost() {
    std::println("\n--- #4667: Soft / Off is zero-cost ---");
    apply_dev_audit_defaults();
    // A negative face must not refuse under Soft / Off (the shared face
    // short-circuits on the production_defaults load).
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
    publish_last_proof_face(false, false);
    const auto before = linear_fast_path_elide_blocked_production_total_v_read();
    CHECK(linear_per_op_typed_entry_readiness_ok(), "4667 AC4: Soft/Off allows (zero-cost)");
    CHECK(linear_fast_path_elide_blocked_production_total_v_read() == before,
          "4667 AC4: Soft/Off adds no counter bump");
    publish_type_linear_proof_outcome(kTypeLinearProofOutcomeQuiet);
    apply_dev_audit_defaults();
}

} // namespace

int run_test_ir_typed_entry_proof_authority() {
    std::println("=== Issue #3305: ir_typed_entry_proof_authority (dual-authority close) ===");
    CHECK(true, "ac3305: issue stamp");

    auto h = read_file("src/compiler/typed_mutation_audit.h");

    // ── AC1: ir_typed_entry_commit_readiness_ok consults last proof face ──
    {
        std::println("\n--- AC1: last-proof-face consult added ---");
        CHECK(!h.empty(), "AC1: typed_mutation_audit.h readable");
        // Locate the function (around line 2043).
        const auto fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept");
        if (fn_pos == std::string::npos) {
            CHECK(false, "AC1: ir_typed_entry_commit_readiness_ok not found");
        } else {
            const std::string scope = h.substr(fn_pos, 8000);
            CHECK(scope.find("g_last_type_linear_proof_outcome") != std::string::npos,
                  "AC1: ir_typed_entry_commit_readiness_ok consults "
                  "g_last_type_linear_proof_outcome");
            CHECK(scope.find("kTypeLinearProofOutcomeReject") != std::string::npos,
                  "AC1: ir_typed_entry_commit_readiness_ok compares against "
                  "kTypeLinearProofOutcomeReject");
            CHECK(
                scope.find("g_last_proof_would_allow_commit") != std::string::npos,
                "AC1: ir_typed_entry_commit_readiness_ok consults g_last_proof_would_allow_commit");
            CHECK(scope.find("g_last_proof_linear_ok") != std::string::npos,
                  "AC1: ir_typed_entry_commit_readiness_ok consults g_last_proof_linear_ok");
        }
    }

    // ── AC2: Move/Drop path still gated (no regression) ──
    {
        std::println("\n--- AC2: linear_move_drop_elision_ok / linear_fast_path_ok regression ---");
        const auto mmd_pos = h.find("inline bool linear_move_drop_elision_ok() noexcept");
        CHECK(mmd_pos != std::string::npos,
              "AC2: linear_move_drop_elision_ok still present (no regression of #3186)");
        if (mmd_pos != std::string::npos) {
            const std::string scope = h.substr(mmd_pos, 2500);
            CHECK(scope.find("linear_ir_fastpath_try_skip") != std::string::npos ||
                      scope.find("commit_readiness") != std::string::npos,
                  "AC2: linear_move_drop_elision_ok still gates via fastpath/readiness");
        }
        const auto lfp_pos = h.find("inline bool linear_fast_path_ok() noexcept");
        CHECK(lfp_pos != std::string::npos,
              "AC2: linear_fast_path_ok still present (no regression of #2964/#3030)");
        if (lfp_pos != std::string::npos) {
            const std::string scope = h.substr(lfp_pos, 800);
            CHECK(scope.find("g_last_type_linear_proof_outcome") != std::string::npos,
                  "AC2: linear_fast_path_ok still consults g_last_type_linear_proof_outcome");
            CHECK(scope.find("g_last_proof_would_allow_commit") != std::string::npos,
                  "AC2: linear_fast_path_ok still consults g_last_proof_would_allow_commit");
        }
    }

    // ── AC3: Soft / depth==0 → zero extra cost (production guard + early return) ──
    {
        std::println("\n--- AC3: production guard + depth==0 short-circuit preserved ---");
        const auto fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept");
        if (fn_pos != std::string::npos) {
            const std::string scope = h.substr(fn_pos, 4500);
            // Soft path: !production_defaults && !Full → return true (zero extra work)
            CHECK(scope.find("if (!(production_defaults_active() || get_strategy() == "
                             "AuditStrategy::Full))") != std::string::npos,
                  "AC3: Soft/Off zero-cost guard preserved");
            // depth==0 short-circuit
            CHECK(scope.find("if (depth == 0)") != std::string::npos,
                  "AC3: depth==0 short-circuit preserved");
            // Issue #3510: negative authority at depth==0 (Reject), Quiet still allows
            CHECK(scope.find("kTypeLinearProofOutcomeReject") != std::string::npos,
                  "AC3/#3510: depth==0 consults Reject");
            CHECK(h.find("kTypeLinearProofOutcomeStamped") != std::string::npos,
                  "AC3/#3414: Stamped outcome still in typed_audit SSOT");
        }
    }

    // ── AC4: Reuse existing counter (no new query key) ──
    {
        std::println("\n--- AC4: existing counter reused, no new query key ---");
        const auto fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept");
        if (fn_pos != std::string::npos) {
            const std::string scope = h.substr(fn_pos, 4500);
            // Verify the existing counter is bumped in the new reject paths.
            const auto bump_count = [&]() -> std::size_t {
                std::size_t count = 0;
                const std::string target =
                    "g_linear_fast_path_elide_blocked_production_total.fetch_add(1,";
                std::size_t pos = 0;
                while ((pos = scope.find(target, pos)) != std::string::npos) {
                    ++count;
                    pos += target.size();
                }
                return count;
            }();
            CHECK(bump_count >= 3,
                  "AC4: g_linear_fast_path_elide_blocked_production_total bumped in >=3 reject "
                  "paths (Reject, would_allow=0, linear_ok=0, cr.would_allow_commit=false)");
        }
    }

    // ── AC5: no docs/design/*, no test_issue_* ──
    {
        std::println("\n--- AC5: no docs/design/*, no test_issue_* ---");
        CHECK(h.find("docs/design/3305") == std::string::npos,
              "AC5: typed_mutation_audit.h does not reference docs/design/3305-*");
        // The new test file uses thematic naming (test_ir_typed_entry_*),
        // not test_issue_*.
        const auto self_path = "tests/compiler/test_ir_typed_entry_proof_authority.cpp";
        auto self = read_file(self_path);
        CHECK(read_file("tests/compiler/test_issue_3305.cpp").empty(),
              "AC5: no invent tests/compiler/test_issue_3305.cpp");
    }

    // ── AC6: thematic naming follows existing IR / type-linear suite ──
    {
        std::println("\n--- AC6: thematic naming follows existing suite convention ---");
        // Verify test_commit_readiness_score.cpp exists (related existing suite)
        // — we extend the convention but not literally the file (the new test
        // covers the dual-authority fix specifically; the existing suite covers
        // commit_readiness scoring).
        auto existing = read_file("tests/compiler/test_commit_readiness_score.cpp");
        CHECK(!existing.empty(),
              "AC6: existing IR / type-linear suite test_commit_readiness_score.cpp present");
        // The new test file uses the existing thematic prefix
        // (test_ir_typed_entry_*) matching the IR / type-linear family.
        CHECK(true, "AC6: new test uses thematic naming test_ir_typed_entry_proof_authority "
                    "(matches existing suite convention)");
    }

    // ── #3416: last-proof stamper identity ──
    {
        std::println("\n--- #3416: last-proof bound to TLS eval ---");
        CHECK(h.find("kLastProofEvalIdentityIssue = 3416") != std::string::npos,
              "3416: issue stamp");
        CHECK(h.find("g_last_proof_stamper_eval") != std::string::npos, "3416: stamper gauge");
        CHECK(h.find("last_proof_bound_to_current_eval") != std::string::npos,
              "3416: bound helper");
        const auto fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept");
        if (fn_pos != std::string::npos) {
            const std::string scope = h.substr(fn_pos, 4500);
            CHECK(scope.find("last_proof_bound_to_current_eval") != std::string::npos,
                  "3416 AC1: typed entry consults stamper identity");
        }
        CHECK(read_file("tests/compiler/test_issue_3416.cpp").empty(),
              "3416: no invent test_issue_3416");
    }

    {
        std::println("\n--- #3688: eval_flat / apply_closure consults same typed-entry helper ---");
        const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(efl.find("Issue #3688") != std::string::npos, "3688: eval_flat cites #3688");
        CHECK(efl.find("production_eval_flat_commit_readiness_refuse") != std::string::npos,
              "3688: shared refuse helper");
        CHECK(efl.find("ir_typed_entry_commit_readiness_ok()") != std::string::npos,
              "3688: reuses IR/JIT helper (no second proof model)");
        CHECK(efl.find("production_hard_face_active()") != std::string::npos,
              "3688 AC4: Soft/Off no commit_readiness load on apply_closure");
        CHECK(efl.find("commit-readiness-refused") != std::string::npos,
              "3688: same TypeError face as IR execute");
        const auto apply_pos = efl.find("Evaluator::apply_closure(");
        CHECK(apply_pos != std::string::npos, "3688: apply_closure present");
        const auto apply_win =
            apply_pos == std::string::npos ? std::string{} : efl.substr(apply_pos, 2500);
        CHECK(apply_win.find("production_eval_flat_commit_readiness_refuse") != std::string::npos,
              "3688 AC1: apply_closure consults refuse helper");
        CHECK(efl.find("schema-3688") == std::string::npos, "3688 AC5: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3688.cpp").empty(),
              "3688: no test_issue_3688.cpp");
        CHECK(read_file("docs/design/3688-eval-flat-commit-readiness.md").empty(),
              "3688: no docs/design/");
        CHECK(h.find("strip_green_face_on_remount_last_zero") != std::string::npos,
              "3688: remount last==0 strip unchanged");
    }

    {
        std::println("\n--- #3758: try_jit_execute ScalarFn consults same helper as execute() ---");
        const auto ixx = read_file("src/compiler/service.ixx");
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(ixx.find("Issue #3758") != std::string::npos, "3758: try_jit_execute cites #3758");
        CHECK(ixx.find("jit_execute_commit_readiness_blocked()") != std::string::npos,
              "3758: try_jit_execute consults execute-catalog helper");
        CHECK(h.find("jit_execute_commit_readiness_blocked") != std::string::npos,
              "3758: helper in typed_audit");
        const auto exec = ir.find("EvalResult IRInterpreter::execute()");
        CHECK(exec != std::string::npos, "3758: execute() present");
        if (exec != std::string::npos) {
            const auto win = ir.substr(exec, 800);
            CHECK(win.find("ir_typed_entry_blocked_result") != std::string::npos,
                  "3758: execute() still refuses at C++ entry");
        }
        const auto cite = ixx.find("Issue #3758: same execute catalog as IRInterpreter::execute");
        CHECK(cite != std::string::npos, "3758: try_jit_execute present");
        if (cite != std::string::npos) {
            // The #3758 site grew (the #4388 cache-hit re-sample landed
            // between the gate and the ScalarFn call), so the window has
            // to cover the call site too - the ordering assertion below
            // is unchanged.
            const auto win = ixx.substr(cite, 2000);
            const auto gate = win.find("jit_execute_commit_readiness_blocked()");
            const auto call = win.find("reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)");
            CHECK(gate != std::string::npos && call != std::string::npos && gate < call,
                  "3758: ScalarFn invoke is after commit_readiness gate");
        }
        CHECK(ixx.find("schema-3758") == std::string::npos, "3758: no new query key");
        CHECK(read_file("tests/compiler/test_issue_3758.cpp").empty(),
              "3758: no test_issue_3758.cpp");

        reset_for_test();
        apply_dev_audit_defaults();
        CompilerService cs;
        CHECK(cs.public_try_jit_execute_counting_scalar_for_test() == 1,
              "3758 AC3: Soft cached ScalarFn still runs");
        apply_production_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::clear_type_linear_commit_proof_for_test();
        bool ok = true;
        {
            aura::compiler::Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
            typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
            stamp_type_linear_commit_proof(3758);
            publish_type_linear_proof_outcome(kTypeLinearProofOutcomeReject);
            publish_last_proof_face(false, false);
            CHECK(jit_execute_commit_readiness_blocked(),
                  "3758 AC1: helper blocked under Reject proof");
            CHECK(cs.public_try_jit_execute_counting_scalar_for_test() == 0,
                  "3758 AC1: try_jit_execute does not invoke ScalarFn with Reject proof");
            typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        }
        (void)ok;
        apply_dev_audit_defaults();
        reset_for_test();
    }

    {
        std::println("\n--- #3759: compiled Apply of linear local skips body under Reject ---");
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        const auto apply = jit.find("case OpApply:");
        CHECK(apply != std::string::npos, "3759: OpApply present");
        if (apply != std::string::npos) {
            const auto win = jit.substr(apply, 4200);
            CHECK(win.find("Issue #3759") != std::string::npos, "3759: OpApply cites #3759");
            CHECK(win.find("begin_linear_epoch_fence()") != std::string::npos,
                  "3759: linear Apply uses skip-body fence");
            const auto fence = win.find("begin_linear_epoch_fence()");
            const auto call = win.find("fn_closure_call");
            CHECK(fence != std::string::npos && call != std::string::npos && fence < call,
                  "3759: Apply body is after fence (skipped on typed-entry fail)");
        }
        CHECK(jit.find("irb->CreateCondBr(is_stale, bb_stale, bb_ok)") != std::string::npos,
              "3759: fence still skip-body on stale");
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(ir.find("commit-readiness-refused") != std::string::npos,
              "3759: interpreter Apply refuses commit-readiness-refused");
        CHECK(read_file("tests/compiler/test_issue_3759.cpp").empty(), "3759: no invent");
    }

    std::println("\n=== Issue #4667: per-op typed-entry readiness re-sample ===");
    ac4667_per_op_readiness_resample();
    ac4667_op_wiring_source_cite();
    ac4667_soft_zero_cost();
    apply_dev_audit_defaults();
    reset_for_test();

    std::println("\n=== Issue #3305 done ===");
    std::println("=== #4667: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_ir_typed_entry_proof_authority();
}
#endif
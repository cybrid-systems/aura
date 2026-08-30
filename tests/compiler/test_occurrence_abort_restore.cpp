// @category: unit
// @reason: Issue #3158 — abort must restore or clear CS occurrence_goals_
// to entry authority (close residual half-green after failed high-freq
// mutate). Closes the gap where abort cleared proof face (#3030) and
// coercion readiness + cone (#3102) but did NOT touch live occurrence_
// goals_ / priority roots, allowing residual narrowing goals to poison
// the next delta's priority + fingerprint + stamp face under production
// Full. Production/Full: capture occurrence_entry_size at guard entry +
// truncate occurrence_goals_ back to that size on abort (after dual-
// topology restore + proof + coercion clear, before any post-abort
// IR/JIT lookup). Soft / Off: bump observe counter only, no structural
// write (zero-cost contract preserved).
//
//   AC1: MutationCheckpoint struct has occurrence_entry_size field
//        captured at guard construction (production/Full only entry path).
//   AC2: ConstraintSystem::restore_or_clear_occurrence_to_entry helper
//        exists + truncates live occurrence_goals_ back to entry size.
//   AC3: All 3 abort sites in evaluator_mutation_boundary.cpp call
//        restore_or_clear_occurrence_to_entry under production/Full AND
//        note_3158_occurrence_abort_observe under Soft/Off (gated on
//        typed_audit::production_defaults_active() || AuditStrategy::Full).
//   AC4: g_3158_occurrence_abort_restore_total + _observe_total counters
//        + note helpers present in typed_mutation_audit.h.
//   AC5: No second model — reuses live occurrence_goals_ table (no
//        parallel goal table); no changes to existing prune_occurrence_goals
//        / clear_blame_context / maybe_persist_occurrence_snapshot.
//   AC6: No docs/design/3158-* plan doc (per #1655 aura 哲学).
//   AC7: No tests/issues/test_issue_3158.cpp (per #81934 — src/-aligned
//        suite instead).
//
// Sibling tests must remain green:
//   - tests/compiler/test_occurrence_goal_persist_rehydrate.cpp (#2910 /
//     #3004 / #3027 / #3082 family)
//   - tests/compiler/test_coercion_map_abort_rewind.cpp (#3102)
//   - tests/compiler/test_type_linear_commit_proof_*.cpp (#3030)

#include "test_harness.hpp"

#include <fstream>
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

// AC1: MutationCheckpoint has occurrence_entry_size field + captured at
// guard construction in evaluator_mutation_boundary.cpp (production/Full
// only path).
static void ac1_entry_size_capture() {
    std::println("\n--- #3158 AC1: occurrence_entry_size captured at guard construction ---");
    const auto evaluator_ixx = read_file("src/compiler/evaluator.ixx");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    CHECK(evaluator_ixx.find("std::size_t occurrence_entry_size = 0") != std::string::npos,
          "AC1: MutationCheckpoint struct has occurrence_entry_size field");

    // Capture at guard construction: cp.occurrence_entry_size = tc->...occurrence_goals_size()
    CHECK(boundary_cpp.find("cp.occurrence_entry_size = ") != std::string::npos,
          "AC1: cp.occurrence_entry_size captured at guard construction");
    const auto capture_pos = boundary_cpp.find("cp.occurrence_entry_size = ");
    if (capture_pos != std::string::npos) {
        // Must read from constraint_system().occurrence_goals_size()
        const auto cs_size_pos =
            boundary_cpp.find("tc->constraint_system().occurrence_goals_size()", capture_pos);
        CHECK(cs_size_pos != std::string::npos,
              "AC1: capture reads tc->constraint_system().occurrence_goals_size()");
    }
}

// AC2: ConstraintSystem::restore_or_clear_occurrence_to_entry helper
// exists + truncates live occurrence_goals_ back to entry size.
static void ac2_restore_helper() {
    std::println("\n--- #3158 AC2: restore_or_clear_occurrence_to_entry helper ---");
    const auto type_checker_ixx = read_file("src/compiler/type_checker.ixx");

    CHECK(type_checker_ixx.find("restore_or_clear_occurrence_to_entry") != std::string::npos,
          "AC2: restore_or_clear_occurrence_to_entry helper declared on ConstraintSystem");
    const auto helper_pos = type_checker_ixx.find("restore_or_clear_occurrence_to_entry");
    if (helper_pos != std::string::npos) {
        // Implementation: occurrence_goals_.resize(entry_size)
        const auto body_marker =
            type_checker_ixx.find("occurrence_goals_.resize(entry_size)", helper_pos);
        CHECK(body_marker != std::string::npos,
              "AC2: helper body truncates via occurrence_goals_.resize(entry_size)");
        // Underflow guard: returns 0 if live <= entry_size
        const auto underflow_marker =
            type_checker_ixx.find("if (live <= entry_size)\n            return 0", helper_pos);
        CHECK(underflow_marker != std::string::npos,
              "AC2: helper underflow guard returns 0 (live <= entry_size no-op)");
    }
}

// AC3: All 3 abort sites in evaluator_mutation_boundary.cpp call the
// restore helper under production/Full AND bump observe counter under
// Soft/Off (gated on typed_audit::production_defaults_active() ||
// AuditStrategy::Full).
static void ac3_all_three_abort_sites() {
    std::println("\n--- #3158 AC3: all 3 abort sites wired ---");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    // Count the number of times restore_or_clear_occurrence_to_entry is called.
    // We expect exactly 3 abort sites + 1 capture (total 4 mentions in
    // evaluator_mutation_boundary.cpp, but the capture is in ctor which
    // uses tc->constraint_system().occurrence_goals_size() — different
    // pattern. So 3 abort-site calls is the expected count.)
    std::size_t count = 0;
    auto p = boundary_cpp.find("restore_or_clear_occurrence_to_entry(");
    while (p != std::string::npos) {
        ++count;
        p = boundary_cpp.find("restore_or_clear_occurrence_to_entry(", p + 1);
    }
    CHECK(count == 3,
          "AC3: restore_or_clear_occurrence_to_entry called from exactly 3 abort sites");

    // Count the number of times note_3158_occurrence_abort_observe is called.
    // Should be 3 (one per abort site, in the else Soft branch).
    std::size_t obs_count = 0;
    auto op = boundary_cpp.find("note_3158_occurrence_abort_observe(");
    while (op != std::string::npos) {
        ++obs_count;
        op = boundary_cpp.find("note_3158_occurrence_abort_observe(", op + 1);
    }
    CHECK(obs_count == 3,
          "AC3: note_3158_occurrence_abort_observe called from exactly 3 abort sites "
          "(Soft / Off path)");

    // Count the number of times note_3158_occurrence_abort_restore is called.
    // Should be 3 (one per abort site, in the production/Full path).
    std::size_t restore_count = 0;
    auto rp = boundary_cpp.find("note_3158_occurrence_abort_restore(");
    while (rp != std::string::npos) {
        ++restore_count;
        rp = boundary_cpp.find("note_3158_occurrence_abort_restore(", rp + 1);
    }
    CHECK(restore_count == 3,
          "AC3: note_3158_occurrence_abort_restore called from exactly 3 abort sites "
          "(production/Full path)");
}

// AC4: New counters + helpers in typed_mutation_audit.h.
static void ac4_counters_and_helpers() {
    std::println("\n--- #3158 AC4: new counters + helpers in typed_mutation_audit.h ---");
    const auto audit_h = read_file("src/compiler/typed_mutation_audit.h");

    CHECK(audit_h.find("g_3158_occurrence_abort_restore_total") != std::string::npos,
          "AC4: g_3158_occurrence_abort_restore_total counter declared");
    CHECK(audit_h.find("g_3158_occurrence_abort_restore_goals_total") != std::string::npos,
          "AC4: g_3158_occurrence_abort_restore_goals_total counter declared");
    CHECK(audit_h.find("g_3158_occurrence_abort_observe_total") != std::string::npos,
          "AC4: g_3158_occurrence_abort_observe_total counter declared");
    CHECK(audit_h.find("note_3158_occurrence_abort_restore(") != std::string::npos,
          "AC4: note_3158_occurrence_abort_restore() helper present");
    CHECK(audit_h.find("note_3158_occurrence_abort_observe(") != std::string::npos,
          "AC4: note_3158_occurrence_abort_observe() helper present");
    CHECK(audit_h.find("occurrence_3158_abort_restore_total_v_read") != std::string::npos,
          "AC4: occurrence_3158_abort_restore_total_v_read() accessor present");
    CHECK(audit_h.find("occurrence_3158_abort_observe_total_v_read") != std::string::npos,
          "AC4: occurrence_3158_abort_observe_total_v_read() accessor present");
}

// AC5: No second model — reuses live occurrence_goals_ table (no
// parallel goal table); no changes to existing prune_occurrence_goals /
// clear_blame_context / maybe_persist_occurrence_snapshot.
static void ac5_no_second_model() {
    std::println("\n--- #3158 AC5: no second model — reuses live occurrence_goals_ table ---");
    const auto type_checker_ixx = read_file("src/compiler/type_checker.ixx");
    const auto audit_h = read_file("src/compiler/typed_mutation_audit.h");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    // AC5: helper operates on occurrence_goals_ directly (no parallel table).
    CHECK(type_checker_ixx.find("occurrence_goals_.resize(entry_size)") != std::string::npos,
          "AC5: restore helper operates on live occurrence_goals_ directly "
          "(no parallel goal table per issue non-goals)");

    // AC5: existing prune_occurrence_goals + clear_blame_context +
    // maybe_persist_occurrence_snapshot unchanged (still present, still called from existing
    // sites).
    CHECK(type_checker_ixx.find("prune_occurrence_goals") != std::string::npos,
          "AC5: existing prune_occurrence_goals helper still present (reuse, not duplicate)");
    CHECK(type_checker_ixx.find("clear_blame_context") != std::string::npos,
          "AC5: existing clear_blame_context still present (still does NOT clear goals — "
          "by design per #3158 AC3)");
    CHECK(
        audit_h.find("maybe_persist_occurrence_snapshot") != std::string::npos ||
            boundary_cpp.find("maybe_persist_occurrence_snapshot") != std::string::npos,
        "AC5: existing maybe_persist_occurrence_snapshot still present (no second persist model)");

    // AC5: no new g_3158_* atomic in type_checker.ixx (counters live in audit header).
    CHECK(type_checker_ixx.find("g_3158_") == std::string::npos,
          "AC5: no g_3158_* atomic counter in type_checker.ixx (counters in audit header)");
}

// AC6 + AC7: no invent docs / no test_issue_3158.cpp (per #1655 / #81934).
static void ac6_7_no_invent_docs() {
    std::println("\n--- #3158 AC6+AC7: no invent docs / no test_issue_3158.cpp ---");
    const auto design = read_file("docs/design/3158-occurrence-abort-restore.md");
    const auto issue_test = read_file("tests/issues/test_issue_3158.cpp");
    CHECK(design.empty(), "AC6: no docs/design/3158-* plan doc (per #1655 aura 哲学)");
    CHECK(issue_test.empty(),
          "AC7: no tests/issues/test_issue_3158.cpp (per #81934 — src/-aligned suite instead)");

    // AC6 + AC7: src/-aligned suite present (this test file).
    const auto this_test = read_file("tests/compiler/test_occurrence_abort_restore.cpp");
    CHECK(!this_test.empty() &&
              this_test.find("run_test_occurrence_abort_restore") != std::string::npos,
          "AC6+AC7: src/-aligned test tests/compiler/test_occurrence_abort_restore.cpp "
          "present");
}

// Issue #3440: persist-reject under outermost && success must flip
// success so the existing !success abort_restore SSOT runs. Do not
// invent a second restore.
static void ac3440_persist_reject_flips_success_into_abort_restore() {
    std::println("\n--- #3440 AC1: persist-reject notes restore + dtor flips success ---");
    const auto audit_h = read_file("src/compiler/typed_mutation_audit.h");
    const auto boundary_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(audit_h.find("kOutermostPersistRejectRestoreIssue = 3440") != std::string::npos,
          "3440 AC1: stamp in typed_mutation_audit.h");
    CHECK(audit_h.find("note_outermost_persist_reject_needs_restore") != std::string::npos,
          "3440 AC1: TLS note helper present");
    CHECK(audit_h.find("consume_outermost_persist_reject_needs_restore") != std::string::npos,
          "3440 AC1: TLS consume helper present");
    CHECK(audit_h.find("g_tls_outermost_persist_reject_needs_restore") != std::string::npos,
          "3440 AC1: TLS flag (not a new metric key)");
    CHECK(audit_h.find("g_3440_") == std::string::npos,
          "3440 AC1: no g_3440_* counter (issue: TLS flag, not a new metric key)");

    const auto fn_pos =
        boundary_cpp.find("extern \"C\" void aura_outermost_success_persist_occurrence(");
    CHECK(fn_pos != std::string::npos, "3440 AC1: persist helper present");
    const auto helper = fn_pos == std::string::npos ? std::string{} : boundary_cpp.substr(fn_pos);
    const auto end_helper = helper.find("extern \"C\" void aura_clear_occurrence_persist_buffer");
    const auto helper_body =
        end_helper == std::string::npos ? helper : helper.substr(0, end_helper);
    std::size_t note_n = 0;
    auto np = helper_body.find("note_3440_restore()");
    while (np != std::string::npos) {
        ++note_n;
        np = helper_body.find("note_3440_restore()", np + 1);
    }
    CHECK(note_n >= 9, "3440 AC1: all persist-reject arms note restore (overflow / unstaged / "
                       "fp-mismatch / mid-abort / drain / pending-face / ADT / last-look / "
                       "recover-fail)");

    std::println("\n--- #3440 AC2: dtor consume + flip success BEFORE exit_mutation_boundary ---");
    const auto persist_call = boundary_cpp.find("aura_outermost_success_persist_occurrence(ev_");
    const auto consume_pos = boundary_cpp.find("consume_outermost_persist_reject_needs_restore()");
    const auto exit_pos = boundary_cpp.find("ev_->exit_mutation_boundary(success)");
    CHECK(persist_call != std::string::npos && consume_pos != std::string::npos &&
              exit_pos != std::string::npos && persist_call < consume_pos && consume_pos < exit_pos,
          "3440 AC2: persist then consume then exit_mutation_boundary (flip before restore)");
    CHECK(boundary_cpp.find("Issue #3440") != std::string::npos, "3440 AC2: dtor cites #3440");
    const auto consume_win =
        consume_pos == std::string::npos ? std::string{} : boundary_cpp.substr(consume_pos, 400);
    CHECK(consume_win.find("success = false") != std::string::npos,
          "3440 AC2: consume arm flips success = false");

    std::println("\n--- #3440 AC3: abort_restore stays SSOT (no second restore) ---");
    std::size_t restore_n = 0;
    auto rp = boundary_cpp.find("abort_restore_dual_topology(");
    while (rp != std::string::npos) {
        ++restore_n;
        rp = boundary_cpp.find("abort_restore_dual_topology(", rp + 1);
    }
    CHECK(restore_n >= 1, "3440 AC3: abort_restore_dual_topology still present");
    CHECK(boundary_cpp.find("abort_restore_dual_topology_persist_reject") == std::string::npos,
          "3440 AC3: no second persist-reject restore helper");
    std::size_t occ_n = 0;
    auto op = boundary_cpp.find("restore_or_clear_occurrence_to_entry(");
    while (op != std::string::npos) {
        ++occ_n;
        op = boundary_cpp.find("restore_or_clear_occurrence_to_entry(", op + 1);
    }
    CHECK(occ_n == 3, "3440 AC3: still exactly 3 #3158 abort sites (reuse, not a 4th restore)");

    std::println("\n--- #3440 AC4: Soft/Off note is a no-op ---");
    CHECK(audit_h.find("Soft/Off note is a no-op") != std::string::npos ||
              audit_h.find("Soft/Off") != std::string::npos,
          "3440 AC4: Soft/Off contract documented on the helper");
    const auto note_fn = audit_h.find("inline void note_outermost_persist_reject_needs_restore");
    CHECK(note_fn != std::string::npos, "3440 AC4: note helper body found");
    const auto note_body =
        note_fn == std::string::npos ? std::string{} : audit_h.substr(note_fn, 500);
    CHECK(note_body.find("production_defaults_active()") != std::string::npos,
          "3440 AC4: note gated on production_defaults_active");
    CHECK(note_body.find("AuditStrategy::Full") != std::string::npos,
          "3440 AC4: note gated on Full strategy");

    std::println("\n--- #3440 AC5: no invent docs / no test_issue_3440.cpp ---");
    CHECK(read_file("docs/design/3440-persist-reject-abort-restore.md").empty(),
          "3440 AC5: no docs/design/3440-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_3440.cpp").empty() &&
              read_file("tests/issues/test_issue_3440.cpp").empty(),
          "3440 AC5: no test_issue_3440.cpp per #81934");
}

} // namespace

int run_test_occurrence_abort_restore() {
    std::println("=== Issue #3158: occurrence abort restore-or-clear to entry authority ===");
    std::println("=== Residual half-green after failed high-freq mutate under production Full ===");
    ac1_entry_size_capture();
    ac2_restore_helper();
    ac3_all_three_abort_sites();
    ac4_counters_and_helpers();
    ac5_no_second_model();
    ac6_7_no_invent_docs();
    ac3440_persist_reject_flips_success_into_abort_restore();

    std::println("\n=== #3158 result: passed={} failed={} ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_abort_restore();
}
#endif

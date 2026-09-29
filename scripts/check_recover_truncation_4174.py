#!/usr/bin/env python3
"""Issue #4174 source-cite gate: recover-true truncation clear must not
survive a commit_readiness forced recovered=false (half-clean CS close).

`try_occurrence_hard_face_full_solve_recover()` clears the CS truncation
stamps BEFORE returning true (note_full_solve_cleared_truncation +
last_partial_cone_truncated_ = false). When commit_readiness then forces
recovered=false (the #3108 re-gate sampling a stale pre-recover
CONFLICT/TIMEOUT solve_status snapshot — or any future post-recover gate),
the denied commit previously kept that cleared state: a half-clean CS the
Agent retry reads as vacuous-SOLVE / missed truncate hard face (stale
narrowing / wrong readiness).

Fix contract: stage a snapshot BEFORE the clear; every #3108 forcing site
rolls the snapshot back on the live commit TC so a denied commit leaves a
consistently LATCHED CS. Accepted recovers keep the clear.

ACs:
  AC1  type_checker.ixx recover SOLVED branch: the truncation restore point
       (note_recover_truncation_snapshot + recover_trunc_pending_ +
       engine-local cone snapshot) is staged BEFORE
       note_full_solve_cleared_truncation / last_partial_cone_truncated_ = false
       — the clear is compensable; cites #4174.
  AC2  type_checker.ixx rollback_occurrence_recover_truncation_clear():
       pending-gated (returns false with nothing staged — idempotent),
       restores BOTH layers (ConstraintSystem snapshot via
       restore_recover_truncation_snapshot AND the engine-local
       last_partial_cone_truncated_ / last_partial_cone_dropped_ stamps),
       and consumes the pending flag; cites #4174.
  AC3  typed_mutation_audit.h: kOccurrenceRecoverTruncationRollbackIssue = 4174
       + g_occurrence_recover_truncation_rollback_total declared, the rollback
       C ABI declared, and ALL THREE #3108 forcing sites
       (`if (aura_typed_audit_recover_truncation_rollback())`) present — step 2
       cone truncate, step 6 cone/empty, step 6c refined_drift. Counter bumps
       only when the ABI reports a consumed snapshot (no noise on hermetic /
       no-live-TC paths).
  AC4  evaluator_mutation_boundary.cpp: aura_typed_audit_recover_truncation_
       rollback() defined, walks aura_typed_audit_current_commit_type_checker(),
       no-op-safe (nullptr guard returns false), propagates
       rollback_occurrence_recover_truncation_clear()'s consumed-pending bool.
  AC5  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it; runtime doors live
       in tests/compiler/test_partial_cone_commit_gate.cpp (ac4174_1..ac4174_5
       defined and registered in run_test_partial_cone_commit_gate);
       no tests/**/test_issue_4174.cpp (per #81934); no docs/design/4174-*
       (per #1655).

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IXX = ROOT / "src" / "compiler" / "type_checker.ixx"
TMH = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TST = ROOT / "tests" / "compiler" / "test_partial_cone_commit_gate.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def main() -> int:
    ixx = IXX.read_text()
    tmh = TMH.read_text()
    emb = EMB.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: restore point staged BEFORE the truncation clear ───────
    solved = ixx.find("if (full == SolveResult::SOLVED) {")
    must(solved >= 0, "AC1: recover SOLVED branch present in type_checker.ixx")
    solved_ret = ixx.find("return true;", solved)
    branch = ixx[solved:solved_ret] if solved >= 0 and solved_ret > solved else ""
    snap = branch.find("note_recover_truncation_snapshot()")
    pending = branch.find("recover_trunc_pending_ = true")
    cone_snap = branch.find("recover_trunc_cone_truncated_ = last_partial_cone_truncated_")
    cs_clear = branch.find("note_full_solve_cleared_truncation()")
    cone_clear = branch.find("last_partial_cone_truncated_ = false")
    must(
        0 <= snap < pending <= cone_snap < cs_clear < cone_clear,
        "AC1: snapshot + cone stamps staged BEFORE the CS/engine-local clear (#4174 ordering)",
    )
    must(
        "Issue #4174" in branch,
        "AC1: SOLVED branch cites #4174 (restore-point rationale inline)",
    )
    must(
        ixx.find("struct RecoverTruncationSnapshot") >= 0,
        "AC1: RecoverTruncationSnapshot restore-point struct declared on the CS",
    )

    # ── AC2: pending-gated rollback restores BOTH layers ────────────
    rb = ixx.find("bool rollback_occurrence_recover_truncation_clear() noexcept {")
    must(rb >= 0, "AC2: TC rollback method present")
    rb_body_start = ixx.find("{", rb)
    rb_body_end = ixx.find("\n    }\n", rb_body_start)
    rb_body = ixx[rb_body_start:rb_body_end] if rb_body_end > rb_body_start else ""
    must(
        "if (!recover_trunc_pending_)" in rb_body and "return false;" in rb_body,
        "AC2: rollback pending-gated (idempotent — false with nothing staged)",
    )
    must(
        "restore_recover_truncation_snapshot()" in rb_body,
        "AC2: rollback restores the ConstraintSystem snapshot layer",
    )
    must(
        "last_partial_cone_truncated_ = recover_trunc_cone_truncated_" in rb_body
        and "last_partial_cone_dropped_ = recover_trunc_cone_dropped_" in rb_body,
        "AC2: rollback restores the engine-local cone truncate stamps",
    )
    must(
        "recover_trunc_pending_ = false" in rb_body and "return true;" in rb_body,
        "AC2: rollback consumes the pending flag and reports the restore",
    )
    must(
        "Issue #4174" in ixx[rb - 1200 : rb] if rb >= 1200 else False,
        "AC2: rollback method cites #4174",
    )
    restore = ixx.find("bool restore_recover_truncation_snapshot() noexcept {")
    must(restore >= 0, "AC2: CS-level restore_recover_truncation_snapshot present")
    restore_body_start = ixx.find("{", restore)
    restore_body_end = ixx.find("\n    }\n", restore_body_start)
    restore_body = ixx[restore_body_start:restore_body_end] if restore_body_end > restore_body_start else ""
    must(
        "recover_trunc_snap_.valid" in restore_body and "last_blame_chain_.truncated_reverify" in restore_body,
        "AC2: CS restore covers the reverify/blame truncation latches and invalidates the snapshot",
    )

    # ── AC3: all three #3108 forcing sites roll the clear back ──────
    must(
        "kOccurrenceRecoverTruncationRollbackIssue = 4174" in tmh,
        "AC3: issue constant declared in typed_mutation_audit.h",
    )
    must(
        "g_occurrence_recover_truncation_rollback_total" in tmh,
        "AC3: rollback counter declared in typed_mutation_audit.h",
    )
    must(
        'extern "C" bool aura_typed_audit_recover_truncation_rollback() noexcept;' in tmh,
        "AC3: rollback C ABI declared at file scope",
    )
    needle = "if (aura_typed_audit_recover_truncation_rollback())"
    sites = tmh.count(needle)
    must(sites == 3, f"AC3: all three #3108 forcing sites roll the clear back (found {sites}/3)")
    step2 = tmh.find("Issue #4174: recover-true already cleared the CS truncation")
    step6 = tmh.find("Issue #4174: same truncation-clear rollback as step 2 — the")
    step6c = tmh.find("Issue #4174: same truncation-clear rollback as step 2 / step 6 —")
    must(
        0 <= step2 < step6 < step6c,
        "AC3: step 2 / step 6 / step 6c each cite #4174 at the forcing site (in order)",
    )
    must(
        "g_occurrence_recover_truncation_rollback_total.fetch_add" in tmh,
        "AC3: counter bumps at the forcing sites (soak / Agent-visible residual)",
    )

    # ── AC4: rollback ABI walks the live commit TC, no-op safe ──────
    abi = emb.find("bool aura_typed_audit_recover_truncation_rollback() noexcept {")
    must(abi >= 0, "AC4: rollback ABI defined in evaluator_mutation_boundary.cpp")
    abi_body_start = emb.find("{", abi)
    abi_body_end = emb.find("\n}", abi_body_start)
    abi_body = emb[abi_body_start:abi_body_end] if abi_body_end > abi_body_start else ""
    must(
        "aura_typed_audit_current_commit_type_checker()" in abi_body,
        "AC4: ABI walks the live commit TC handle (no process-global fn/ctx)",
    )
    must(
        abi_body.count("if (") >= 2 and "return false;" in abi_body,
        "AC4: ABI is no-op-safe (nullptr TC / nothing pending → false)",
    )
    must(
        "return tc->rollback_occurrence_recover_truncation_clear();" in abi_body,
        "AC4: ABI propagates the consumed-pending bool from the TC rollback",
    )
    must(
        "Issue #4174" in emb[abi - 1500 : abi] if abi >= 1500 else False,
        "AC4: ABI definition cites #4174",
    )

    # ── AC5: wiring + runtime doors ─────────────────────────────────
    must(
        "check_recover_truncation_4174.py" in build,
        "AC5: build.py registers the linter",
    )
    must(
        "check_recover_truncation_4174.py" in allow,
        "AC5: root_check_allowlist.txt lists the linter",
    )
    for fn in (
        "ac4174_1_rollback_restores_truncation",
        "ac4174_2_forced_reject_rolls_back_live_tc",
        "ac4174_3_accept_keeps_clear",
        "ac4174_4_hermetic_forced_reject_no_bump",
        "ac4174_5_source_cite_and_linter",
    ):
        defined = tst.find(f"static void {fn}()") >= 0
        registered = tst.find(f"{fn}();") >= 0
        must(defined and registered, f"AC5: runtime door {fn} defined + registered in run_test")
    must(
        not (ROOT / "tests" / "compiler" / "test_issue_4174.cpp").exists(),
        "AC5: no tests/**/test_issue_4174.cpp (per #81934)",
    )
    must(
        not any((ROOT / "docs" / "design").glob("4174-*")),
        "AC5: no docs/design/4174-* (per #1655)",
    )

    if failures:
        print(f"\ncheck_recover_truncation_4174: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_recover_truncation_4174: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# scripts/check_occurrence_recover_regate_3623.py -- Issue #3623 source-cite gate.
#
# AC1: commit_readiness step 6c (refined_drift) carries the #3108 SOLVED
#      re-gate — the recover hook result is re-gated on the
#      CommitReadinessInput solve_status snapshot exactly like the cone
#      (step 2) and cone/empty (step 6) faces. An override recover that
#      reports true while the CS snapshot is CONFLICT/TIMEOUT must fail
#      closed, never stamp would_allow_commit.
# AC2: reject face unchanged — force_reason refined_drift (code 15,
#      bp 750) on recover fail / non-SOLVED.
# AC3: Soft observe path unchanged (g_refined_consistency_observe_total;
#      refined_consistency_hard=false stays allow).
# AC4: cone/empty #3108 re-gates untouched (same two lines, no behavior
#      change there); no new counter, no new query key — reuses
#      g_occurrence_recover_not_solved_total.
# AC5: runtime ACs in tests/compiler/test_partial_cone_commit_gate.cpp
#      (ac3623_*, link-time mock recover seam); no test_issue_3623.cpp,
#      no docs/design/*3623*.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

H = "src/compiler/typed_mutation_audit.h"
TEST = "tests/compiler/test_partial_cone_commit_gate.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_occurrence_recover_regate_3623"
REGATE = "recovered && in.solve_status != 0"
BUMP = "g_occurrence_recover_not_solved_total.fetch_add(1"
CITE = "Issue #3623: #3108 SOLVED re-gate"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(h: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — 6c carries the #3108 SOLVED re-gate (enumerative site count).
    must(CITE, "AC1 6c re-gate cite", h)
    n_regate = h.count(REGATE)
    if n_regate < 3:
        fails.append(f"AC1: expected >=3 re-gate sites (cone/6c/empty), found {n_regate}")
    n_bump = h.count(BUMP)
    if n_bump < 3:
        fails.append(f"AC1: expected >=3 re-gate bump sites, found {n_bump}")
    # The 6c window between the face comment and the next face.
    c6 = h.find("6c) Issue #2911")
    c6d = h.find("6d) Issue #3031")
    if c6 == -1 or c6d == -1 or c6d < c6:
        fails.append("AC1: 6c window not found")
        win = ""
    else:
        win = h[c6:c6d]
        must(
            "bool recovered = aura_typed_audit_try_occurrence_hard_face_full_solve_recover()",
            "AC1 recover hook call in 6c",
            win,
        )
        must(REGATE, "AC1 re-gate inside 6c", win)
        must(BUMP, "AC1 bump inside 6c", win)

    # AC2 — reject face unchanged.
    must('set("refined_drift", false, 750)', "AC2 refined_drift reject bp 750", h)
    must("g_refined_consistency_reject_total", "AC2 reject counter kept", h)
    must("g_refined_consistency_recover_total", "AC2 recover counter kept", h)

    # AC3 — Soft observe path unchanged.
    must("g_refined_consistency_observe_total", "AC3 soft observe counter", h)

    # AC4 — cone/empty re-gates untouched; no new counter / query key.
    must("Issue #3108: re-gate recover on solve_status==SOLVED (0)", "AC4 cone re-gate cite", h)
    must("Issue #3108: second re-gate", "AC4 cone/empty re-gate cite", h)
    must_not("g_3623_", "AC4 no new counter", h)
    must_not("schema-3623", "AC4 no new query key", h)

    # AC5 — runtime ACs in the existing gate suite.
    for fn in (
        "ac3623_1_recover_solved_allows",
        "ac3623_2_recover_fail_rejects",
        "ac3623_3_conflict_snapshot_fail_closed",
        "ac3623_4_soft_observe_allow",
    ):
        must(fn, "AC5 test AC defined", test)
        must(fn + "();", "AC5 test AC invoked", test)
    must("g_ac3623_mock_recover", "AC5 mock recover seam", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3623.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3623.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "3623" in p.name:
                fails.append(f"AC4: forbidden docs/design/{p.name} (#1655)")
                break

    # Wiring.
    must(LINTER, "wiring build.py", build)
    must(LINTER + ".py", "wiring allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_h = (
            "6c) Issue #2911: unified refined-consistency hard gate.\n"
            "bool recovered = aura_typed_audit_try_occurrence_hard_face_full_solve_recover()\n"
            + CITE
            + "\n"
            + REGATE
            + "\n"
            + BUMP
            + "\n"  # cone (step 2)
            + REGATE
            + "\n"
            + BUMP
            + "\n"  # cone/empty (step 6)
            + REGATE
            + "\n"
            + BUMP
            + "\n"  # refined_drift (step 6c, #3623)
            "Issue #3108: re-gate recover on solve_status==SOLVED (0)\n"
            "Issue #3108: second re-gate\n"
            'set("refined_drift", false, 750)\n'
            "g_refined_consistency_reject_total\n"
            "g_refined_consistency_recover_total\n"
            "g_refined_consistency_observe_total\n"
            "6d) Issue #3031: pending_full_solve / locality residual.\n"
        )
        sample_test = (
            "".join(
                fn + "\n" + fn + "();\n"
                for fn in (
                    "ac3623_1_recover_solved_allows",
                    "ac3623_2_recover_fail_rejects",
                    "ac3623_3_conflict_snapshot_fail_closed",
                    "ac3623_4_soft_observe_allow",
                )
            )
            + "g_ac3623_mock_recover\n"
        )
        ok_fails = _rows(sample_h, sample_test, LINTER, LINTER + ".py")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "")
        if len(neg_fails) < 12:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(H), _read(TEST), _read(BUILD), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("occurrence recover re-gate (#3623) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

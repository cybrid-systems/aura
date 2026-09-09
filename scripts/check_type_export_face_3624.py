#!/usr/bin/env python3
# scripts/check_type_export_face_3624.py -- Issue #3624 source-cite gate.
#
# AC1: TypeChecker::type_export_is_authoritative() consults the pending
#      residual face (type_export_residual_faces_clear +
#      type_export_residual_faces_stable) — the same face model the
#      Evaluator accessor has consulted since #3237/#3316. #3307
#      budget-allow keeps local SOLVED; before #3624 only the Evaluator
#      consulted the face, so TypeChecker-level export/query
#      (last_occurrence_vars, commit_cs_live, copy_infer authority)
#      still exported refined TypeIds mid-batch.
# AC2: restore point unchanged — the #3190 drain
#      (drain_pending_full_solve_before_commit) clears the face on SOLVED
#      (note_pending_full_solve_residual(0, true)) and the #3307
#      budget-allow hard latch is retained (note(..., /*hard=*/true)).
# AC3: last_occurrence_vars() authority gate intact — accessor-false
#      returns the shared empty vector (no live refined TypeId from a
#      half-solved CS).
# AC4: ordering — status compare precedes the face consult; the Soft
#      observe arm stays observe-only (no note call, no hard latch).
# AC5: runtime ACs live in tests/compiler/test_solve_delta_unresolved_export.cpp
#      (ac3624_1..5); no docs/design/*3624*, no tests/**/test_issue_3624.cpp;
#      linter registered in build.py.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

IXX = "src/compiler/type_checker.ixx"
IMPL = "src/compiler/type_checker_impl.cpp"
TEST = "tests/compiler/test_solve_delta_unresolved_export.cpp"
BUILD = "build.py"

LINTER = "check_type_export_face_3624"
CLEAR = "type_export_residual_faces_clear()"
STABLE = "type_export_residual_faces_stable()"
CITE = "Issue #3624"
ANCHOR = "if (last_delta_solve_status_ != SolveResult::SOLVED)"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(ixx: str, impl: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — TC accessor consults the residual face (same model as Evaluator).
    pos = ixx.find(ANCHOR)
    if pos == -1:
        fails.append("AC1: TypeChecker accessor status compare not found")
        win = ""
    else:
        # The #3624 cite lives in the comment block above the accessor.
        win = ixx[max(0, pos - 800) : pos + 1200]
        must(CITE, "AC1 #3624 cite in accessor", win)
        must(CLEAR, "AC1 residual-face clear consult", win)
        must(STABLE, "AC1 residual-face stable consult", win)
        must(
            "if (!last_type_export_authoritative_)",
            "AC1 flag consult retained",
            win,
        )

    # AC2 — restore point: drain SOLVED clears face; #3307 latch retained.
    must("note_pending_full_solve_residual(0, true)", "AC2 drain SOLVED clears face", impl)
    must(
        "note_pending_full_solve_residual(residual, /*hard=*/true)",
        "AC2 #3307 budget-allow hard latch retained",
        impl,
    )

    # AC3 — last_occurrence_vars authority gate intact.
    lo = ixx.find("const std::vector<aura::core::TypeId>& last_occurrence_vars() const noexcept")
    if lo == -1:
        fails.append("AC3: last_occurrence_vars accessor not found")
    else:
        low = ixx[lo : lo + 480]
        must(
            "if (!type_export_is_authoritative())",
            "AC3 authority gate in occurrence vars",
            low,
        )
        must("kEmpty", "AC3 shared empty return", low)

    # AC4 — ordering + Soft arm unchanged (same anchor as ac3307_2).
    if win:
        i_status = win.find(ANCHOR)
        i_clear = win.find(CLEAR)
        if i_clear != -1 and i_clear < i_status:
            fails.append("AC4: face consult must follow the status compare")
    hard_pos = impl.find("if (!hard) {")
    if hard_pos == -1:
        fails.append("AC4: Soft arm not found")
    else:
        soft = impl[hard_pos : hard_pos + 600]
        must("solve_delta_locality_slo_observe_total", "AC4 Soft observe counter", soft)
        must("return prior;", "AC4 Soft arm returns SOLVED", soft)
        must_not(
            "note_pending_full_solve_residual(",
            "AC4 Soft arm stays observe-only",
            soft,
        )

    # AC5 — runtime ACs + registration + no docs/design + no invented test.
    for fn in (
        "ac3624_1_budget_allow_denies_tc_export",
        "ac3624_2_drain_solved_restores_tc_export",
        "ac3624_3_drain_nonsolved_stays_denied",
        "ac3624_4_soft_observe_unchanged",
        "ac3624_5_source_and_linter",
    ):
        must(fn, "AC5 runtime AC", test)
    must("check_type_export_face_3624", "AC5 build.py registration", build)
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3624*"))
        if hits:
            fails.append(f"AC5: docs/design/*3624* present: {hits}")
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3624.cpp"))
    if invented:
        fails.append(f"AC5: invented test_issue_3624.cpp present: {invented}")
    return fails


def _self_test() -> int:
    ixx_ok = (
        "bool type_export_is_authoritative() const noexcept {\n"
        "        if (last_delta_solve_status_ != SolveResult::SOLVED)\n"
        "            return false;\n"
        "        if (!last_type_export_authoritative_)\n"
        "            return false;\n"
        "        // Issue #3624: drop export authority until drain SOLVED.\n"
        "        if (!type_export_residual_faces_clear())\n"
        "            return false;\n"
        "        return type_export_residual_faces_stable();\n"
        "    }\n"
        "    const std::vector<aura::core::TypeId>& last_occurrence_vars() const noexcept {\n"
        "        if (!type_export_is_authoritative()) {\n"
        "            static const std::vector<aura::core::TypeId> kEmpty;\n"
        "            return kEmpty;\n"
        "        }\n"
        "        return last_occurrence_vars_;\n"
        "    }\n"
    )
    impl_ok = (
        "ConstraintSystem::escalate_locality_slo_if_production(SolveResult prior) {\n"
        "    if (!hard) {\n"
        "        c.solve_delta_locality_slo_observe_total.fetch_add(1, std::memory_order_relaxed);\n"
        "        return prior;\n"
        "    }\n" + ("// padding\n" * 60) + "        note_pending_full_solve_residual(residual, /*hard=*/true);\n"
        "        note_pending_full_solve_residual(0, true);\n"
    )
    test_ok = " ".join(
        f"static void ac3624_{i}_{n}() {{}}"
        for i, n in enumerate(
            (
                "budget_allow_denies_tc_export",
                "drain_solved_restores_tc_export",
                "drain_nonsolved_stays_denied",
                "soft_observe_unchanged",
                "source_and_linter",
            ),
            start=1,
        )
    )
    build_ok = 'ROOT / "scripts" / "check_type_export_face_3624.py"'

    good = _rows(ixx_ok, impl_ok, test_ok, build_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1
    stripped_ixx = ixx_ok.replace("if (!type_export_residual_faces_clear())\n            return false;\n", "")
    bad = _rows(stripped_ixx, impl_ok, test_ok, build_ok)
    if not any("clear consult" in r for r in bad):
        print("self-test: stripped fixture did not trip the AC1 clear-consult row")
        return 1
    print("ok check_type_export_face_3624 self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3624 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(IXX), _read(IMPL), _read(TEST), _read(BUILD))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3477: dirty-skip / cap-truncate leftover after solve_delta
must latch the #3031 pending residual face before IR can ride the last
green stamp. Persist #3190 drain remains stamp authority.

Contract:
  AC1  production leftover → note_pending_full_solve_residual(hard) +
       clear last Stamped proof after escalate_locality_slo
  AC2  drain_pending_full_solve_before_commit still note(0, true)
  AC3  leftover gate (truncated || dirty || pending) — quiet skips
  AC4  Soft hard=false; no schema-3477 / g_3477_*
  AC5  extend named tests; no invent
  AC6  linter AFTER #3190

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    impl = _read("src/compiler/type_checker_impl.cpp")
    test_health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    test_heavy = _read("tests/compiler/test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp")
    build = _read("build.py")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")

    start = impl.find("SolveResult ConstraintSystem::solve_delta(")
    end = impl.find("SolveResult ConstraintSystem::solve_delta_impl(")
    if start < 0 or end < 0 or end <= start:
        fails.append("AC6: solve_delta wrapper window missing")
        win = ""
    else:
        win = impl[start:end]
        must("Issue #3477", "AC6 cite", win)
        must("last_reverify_truncated_", "AC1 leftover truncated", win)
        must("dirty_count_", "AC1 leftover dirty", win)
        must("pending_full_solve_roots_", "AC1 leftover pending", win)
        must("note_pending_full_solve_residual", "AC1 latch", win)
        must("clear_type_linear_commit_proof_on_abort", "AC1 proof-clear", win)
        must("kTypeLinearProofOutcomeStamped", "AC1 Stamped gate", win)
        loc = win.find("escalate_locality_slo_if_production")
        note = win.find("note_pending_full_solve_residual")
        if loc < 0 or note < 0 or note < loc:
            fails.append("AC1: latch must follow escalate_locality_slo_if_production")
        hard = win.find("if (hard)")
        clr = win.find("clear_type_linear_commit_proof_on_abort")
        if hard < 0 or clr < 0 or clr < hard:
            fails.append("AC4: proof-clear must sit inside production/Full hard branch")

    drain = impl.find("ConstraintSystem::drain_pending_full_solve_before_commit")
    if drain < 0:
        fails.append("AC2: drain helper missing")
    else:
        drain_win = impl[drain : drain + 4500]
        must("note_pending_full_solve_residual(0, true)", "AC2 drain clear", drain_win)
    must("drain_pending_full_solve_before_commit", "AC2 persist drain", emb)
    must("aura_outermost_success_persist_occurrence", "AC2 persist stamp", emb)

    must("3477 AC1: residual face set before next IR typed-entry", "AC5 AC1 health", test_health)
    must("3477 AC1: IR typed-entry refused even if previous proof was Stamped", "AC5 AC1 health IR", test_health)
    must("3477 AC2: persist #3190 drain still stamp authority", "AC5 AC2", test_health)
    must("3477 AC3: no face latch", "AC5 AC3", test_health)
    must("3477 AC4: Soft no hard face", "AC5 AC4", test_health)
    must("3477 AC1: residual face set before next IR typed-entry", "AC5 AC1 heavy", test_heavy)
    must("3477 AC4: Soft no hard face", "AC5 AC4 heavy", test_heavy)

    must_not("schema-3477", "AC4 no query key", impl)
    must_not("g_3477_", "AC4 no g_3477_*", impl)

    must("check_solve_delta_dirty_skip_residual_3477", "AC6 build.py", build)
    prev = build.find("check_residual_drain_outermost_stamp_3190")
    ours = build.find("check_solve_delta_dirty_skip_residual_3477")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3190")

    if (ROOT / "tests" / "compiler" / "test_issue_3477.cpp").is_file():
        fails.append("AC5: test_issue_3477.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3477-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3477 solve_delta_dirty_skip_residual:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3477 solve_delta_dirty_skip_residual: latch after solve_delta; persist drain kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())

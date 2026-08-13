#!/usr/bin/env python3
"""Issue #2962: production hard-reject when cone-truncate + outside drop recover fails.

Residual of #2909: recover success must leave SOLVED (solve_status==0);
otherwise force_reason cone_outside_goal_drop. Soft observe-only; quiet zero cost.

Contract:
  AC1 Production + truncate + outside drop → recover once; SOLVED only allows;
      fail/non-SOLVED → hard-reject force_reason cone_outside_goal_drop
  AC2 Soft observe only; no hard-reject counter
  AC3 Quiet no-truncate: zero force-closure attempt
  AC4 schema-2962 + recover-ok/reject totals; lineage #2909/#2703/#2750
  AC5 Extend test_partial_cone_commit_gate; type_checker + tma cites; no docs/design/*
  AC6 Linter + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    tc = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    q = read_query_prims()
    test = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1
    must("#2962", "AC1", tma)
    must("kConeOutsideGoalDropRecoverRejectIssue", "AC1", tma)
    must("g_cone_outside_goal_drop_recover_ok_total", "AC1", tma)
    must("g_cone_outside_goal_drop_reject_total", "AC1", tma)
    must("solve_status != 0", "AC1", tma)
    must("cone_outside_goal_drop", "AC1", tma)
    must("ac2962_1_recover_non_solved_hard_reject", "AC1", test)

    # AC2
    must("Soft vs production decision table", "AC2", tma)
    must("ac2962_3_soft_quiet", "AC2", test)

    # AC3
    must("ac2962_3_soft_quiet", "AC3 quiet", test)
    must("quiet no force-closure attempt", "AC3", test)  # CHECK message in suite

    # AC4
    must("schema-2962", "AC4", q)
    must("cone-outside-goal-drop-recover-ok-total", "AC4", q)
    must("cone-outside-goal-drop-reject-total", "AC4", q)
    must("schema-2909", "AC4 lineage", q)
    must("schema-2703", "AC4 lineage", q)
    must("schema-2750", "AC4 lineage", q)

    # AC5
    must("#2962", "AC5", tc)
    must("publish_cone_outside_goal_drop", "AC5", impl)
    must("check_cone_outside_goal_drop_recover_reject_2962", "AC5", build)
    must("ac2962_4_schema_and_source", "AC5", test)
    must("ac2962_5_linter_no_design", "AC5", test)

    # AC6
    must("cmd_cone_outside_goal_drop_recover_reject_2962", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*2962*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2962.cpp").is_file():
        fails.append("tests/compiler/test_issue_2962.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2962 cone-outside-goal-drop recover SOLVED gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

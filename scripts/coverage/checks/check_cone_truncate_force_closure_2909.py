#!/usr/bin/env python3
"""Issue #2909: force dependency closure / full-solve recover on cone truncate + outside drop.

Contract:
  AC1 Production + truncate + outside-If drop → recover or hard reject (no silent green)
  AC2 Soft observe only; quiet no-truncate zero cost
  AC3 publish_cone_outside_goal_drop wired from infer_flat_partial
  AC4 schema-2909 + force-closure counters; lineage #2560/#2621/#2703/#2716/#2750
  AC5 extend test_partial_cone_commit_gate; no docs/design/*
  AC6 Soft vs production decision table in code comments

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    impl = _read("src/compiler/type_checker_impl.cpp")
    tc = _read("src/compiler/type_checker.ixx")
    q = read_query_prims()
    test = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1
    must("#2909", "AC1", tma)
    must("g_cone_truncate_force_closure_total", "AC1", tma)
    must("g_cone_truncate_force_closure_attempt_total", "AC1", tma)
    must("g_cone_truncate_force_closure_reject_total", "AC1", tma)
    must("outside_drop", "AC1", tma)
    must("cone_outside_goal_drop", "AC1", tma)
    must("ac2909_1_production_force_recover_or_reject", "AC1", test)

    # AC2
    must("Soft + truncate + outside drop", "AC2", tma)
    must("ac2909_2_soft_observe_only", "AC2", test)
    must("ac2909_3_quiet_zero_cost", "AC2", test)

    # AC3
    must("publish_cone_outside_goal_drop", "AC3", tma)
    must("publish_cone_outside_goal_drop", "AC3", impl)

    # AC4
    must("schema-2909", "AC4", q)
    must_key("cone-truncate-force-closure-total", "AC4", q)
    must("kConeTruncateForceClosureIssue", "AC4", tma)
    must("schema-2703", "AC4", q)
    must("schema-2750", "AC4", q)
    must("schema-2621", "AC4", q)

    # AC5
    must("#2909", "AC5", tc)
    must("try_goal_priority_reverify_before_full", "AC5", tc)
    must("check_cone_truncate_force_closure_2909", "AC5", build)
    must("cmd_cone_truncate_force_closure_2909", "AC5", build)
    must("ac2909_5_source_cite", "AC5", test)

    # AC6
    must("decision table", "AC6", tma)
    must("ac2909_6_linter_and_no_design", "AC6", test)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2909-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2909.cpp").is_file():
        fails.append("tests/compiler/test_issue_2909.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2909 cone truncate force-closure — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

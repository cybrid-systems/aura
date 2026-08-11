#!/usr/bin/env python3
"""Issue #2911: refined-consistency hard gate on commit_readiness.

Contract:
  AC1 production + refined drift → would_allow_commit=false (refined_drift)
  AC2 Soft observe only; recover clears face
  AC3 quiet path zero cost
  AC4 schema-2911 + counters; #2553/#2697/#2842 preserved
  AC5 extend type-linear suite; note_refined from composite path
  AC6 decision table + linter; no docs/design/*

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1
    must("#2911", "AC1", tma)
    must("refined_drift", "AC1", tma)
    must("return 15", "AC1", tma)
    must("refined_consistency_hard", "AC1", tma)
    must("refined_consistency_drift", "AC1", tma)
    must("g_refined_consistency_drift_face", "AC1", tma)
    must("ac2911_1_production_reject_on_drift", "AC1", t)

    # AC2
    must("Soft + drift", "AC2", tma)
    must("g_refined_consistency_observe_total", "AC2", tma)
    must("g_refined_consistency_recover_total", "AC2", tma)
    must("ac2911_2_soft_observe_and_recover", "AC2", t)

    # AC3
    must("ac2911_3_quiet_zero_cost", "AC3", t)

    # AC4
    must("schema-2911", "AC4", q)
    must("refined-consistency-wired", "AC4", q)
    must("refined-consistency-reject-total", "AC4", q)
    must("schema-2553", "AC4", q)
    must("kRefinedConsistencyGateIssue", "AC4", tma)

    # AC5
    must("note_refined_consistency_drift", "AC5", etc)
    must("#2911", "AC5", etc)
    must("check_refined_consistency_commit_gate_2911", "AC5", build)
    must("cmd_refined_consistency_commit_gate_2911", "AC5", build)
    must("ac2911_5_source_cite", "AC5", t)

    # AC6
    must("decision table", "AC6", tma)
    must("ac2911_6_linter_and_decision_table", "AC6", t)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2911-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2911.cpp").is_file():
        fails.append("tests/compiler/test_issue_2911.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2911 refined-consistency commit gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

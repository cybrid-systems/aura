#!/usr/bin/env python3
"""Issue #2910: production persist always-on + densify/steal stamp after rehydrate.

Contract:
  AC1 production + goals → append without env (always-on success)
  AC2 Soft / empty → zero cost
  AC3 densify fence before TypeLinearCommitProof freeze; steal rehydrates
  AC4 rehydrate restores non-empty CS goals for #2842 stamp truth
  AC5 schema-2910 + lineage #2608/#2704/#2842/#2896; extend persist suite
  AC6 Soft vs production decision table; no docs/design/*

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

    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    # AC1
    must("#2910", "AC1", impl)
    must("production_defaults_active", "AC1", impl)
    must("AuditStrategy::Full", "AC1", impl)
    must("aura_outermost_success_persist_occurrence", "AC1", mb)
    must("ac2910_1_production_always_persist", "AC1", t)

    # AC2
    must("Soft + goals", "AC2", impl)
    must("ac2910_2_soft_zero_cost", "AC2", t)

    # AC3 — densify order: fence before densify_goal_truth_2842
    fence = mb.find("note_type_freshness_after_steal_or_densify()")
    freeze = mb.find("densify_goal_truth_2842")
    if fence < 0 or freeze < 0 or fence >= freeze:
        fails.append("AC3: densify fence must appear before densify_goal_truth_2842 freeze")
    must("#2910", "AC3", mb)
    must("rehydrate_occurrence_from_persist", "AC3", efm)
    must("ac2910_3_stamp_after_rehydrate_order", "AC3", t)

    # AC4
    must("rehydrate_occurrence_from_persist", "AC4", ixx)
    must("ac2910_4_goal_truth_after_rehydrate", "AC4", t)

    # AC5
    must("schema-2910", "AC5", q)
    must_key("occurrence-persist-stamp-after-rehydrate-wired", "AC5", q)
    must_key("occurrence-persist-production-always-on-success", "AC5", q)
    must("schema-2896", "AC5", q)
    must("schema-2608", "AC5", q)
    must("check_occurrence_persist_production_2910", "AC5", build)
    must("cmd_occurrence_persist_production_2910", "AC5", build)

    # AC6
    must("decision table", "AC6", impl)
    must("ac2910_6_linter_and_decision_table", "AC6", t)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2910-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2910.cpp").is_file():
        fails.append("tests/compiler/test_issue_2910.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2910 occurrence persist production + stamp-after-rehydrate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

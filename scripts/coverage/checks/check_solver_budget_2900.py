#!/usr/bin/env python3
"""Issue #2900: SolverBudget Agent-controlled delta TIMEOUT policy.

Contract:
  AC1 Soft + allow_timeout_commit + TIMEOUT → export total; not SOLVED
  AC2 Production + budget → still escalate (#2277); cannot half-ship
  AC3 Default budget → current behavior
  AC4 Additive schema-2900; preserve #2277/#2107
  AC5 Source-cite + extend test_solve_delta_unresolved_export; no docs/design/
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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("SolverBudget", "AC1", ixx)
    must("allow_timeout_commit", "AC1", ixx)
    must("set_solver_budget", "AC1", ixx)
    must("allow_timeout_commit", "AC1", impl)
    must("solver_budget_timeout_export_total", "AC1", impl)

    must("solver_budget_full_escalate_total", "AC2", impl)
    must("escalate_if_production", "AC2", impl)
    must("production_defaults_active", "AC2", impl)

    must("is_default", "AC3", ixx)
    must("kSolverBudgetDefault", "AC3", ixx)

    must("schema-2900", "AC4", q)
    must("solver-budget-wired", "AC4", q)
    must("schema-2277", "AC4", q)
    must("solver_budget_timeout_export_total", "AC4", aud)

    must("ac2900_1_soft_allow_timeout_export", "AC5", t)
    must("ac2900_2_production_still_escalates", "AC5", t)
    must("ac2900_3_default_budget_unchanged", "AC5", t)
    must("ac2900_4_additive_query", "AC5", t)
    must("ac2900_5_source_cite", "AC5", t)
    must("check_solver_budget_2900", "AC5", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2900-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2900.cpp").is_file():
        fails.append("tests/compiler/test_issue_2900.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2900 SolverBudget — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

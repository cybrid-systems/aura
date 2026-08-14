#!/usr/bin/env python3
"""Issue #2994: Agent-controlled locality residual budget.

Production escalate_locality_slo_if_production keeps SOLVED when
residual ≤ SolverBudget.max_locality_residual and hands residual
roots to pending_full_solve. Default 0 = #2913 escalate.

Contract:
  AC1 Default budget 0 → #2913 escalate
  AC2 residual ≤ N → SOLVED + pending handoff
  AC3 residual > N → full escalate
  AC4 Soft: no full solve; no new budget counters
  AC5 Quiet residual 0: no new atomics
  AC6 is_default includes new fields; schema-2994
  AC7 extend test_solve_delta_unresolved_export; no docs/design / invent

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    met = _read("src/compiler/observability_metrics.h")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("escalate_locality_slo_if_production", "AC1", ixx)
    must("escalate_locality_slo_if_production", "AC1", impl)
    must("max_locality_residual", "AC1", ixx)
    must("ac2994_1_default_budget_escalate", "AC1", t)

    must("prefer_pending_roots_next", "AC2", ixx)
    must("handoff_locality_residual_to_pending", "AC2", impl)
    must("pending_full_solve_roots_", "AC2", impl)
    must("ac2994_2_budget_allow_pending_handoff", "AC2", t)
    must("force_locality_pruned_for_test", "AC2", t)

    must("delta_locality_budget_escalate_total", "AC3", impl)
    must("ac2994_3_budget_over_escalate", "AC3", t)

    must("ac2994_4_soft_no_budget_counters", "AC4", t)
    must("solve_delta_locality_slo_observe_total", "AC4", impl)

    must("ac2994_5_quiet_zero_cost", "AC5", t)
    must("last_locality_pruned_ == 0", "AC5", impl)

    must("max_locality_residual == 0 && prefer_pending_roots_next", "AC6", ixx)
    must("kSolverBudgetLocalityIssue", "AC6", ixx)
    must_key("schema-2994", "AC6", q)
    must_key("delta-locality-budget-allow-total", "AC6", q)
    must("delta_locality_budget_allow_total", "AC6", aud)
    must("delta_locality_budget_allow_total", "AC6", met)
    must("ac2994_6_is_default_and_schema", "AC6", t)

    must("ac2994_7_source_cite", "AC7", t)
    must("check_solve_delta_locality_budget_2994", "AC7", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2994.cpp").is_file():
        fails.append("AC7: test_issue_2994.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2994-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2994 locality residual budget — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2934: MutationBoundaryGuard exit restamp budget + Agent metrics.

Contract (one row per AC):
  AC1 restamp budget (nodes) configurable process/env; Guard exit uses it
  AC2 over budget → soft-degrade (incremental/lazy); never silent torn gen
  AC3 Agent-visible restamp-budget / exceeded / skipped / last-exceeded keys
  AC4 default unlimited Soft regression green
  AC5 source-cite evaluator.ixx / mutation_boundary / ast / flatast_restamp
  AC6 no docs/design/*; linter-checkable

Issue #3041 successor: production budget exceed also forces QueryEpoch
stale (check_restamp_budget_query_epoch_stale_3041.py).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    restamp = _read("src/core/flatast_restamp.hh")
    astx = _read("src/core/ast.ixx")
    impl = _read("src/core/ast_impl.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    eix = _read("src/compiler/evaluator.ixx")
    stdlib = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_restamp_sla_observability.cpp")
    build = _read("build.py")

    # AC1
    must("AURA_RESTAMP_BUDGET_NODES", "AC1", restamp)
    must("kRestampBudgetIssue = 2934", "AC1", restamp)
    must("restamp_budget_nodes_effective", "AC1", restamp)
    must("restamp_budget_nodes_effective", "AC1 body", impl)
    must("Issue #2934", "AC1", impl)
    must("restamp_budget_nodes()", "AC1", astx)

    # AC2 soft-degrade
    must("restamp_budget_exceeded_total_", "AC2", impl)
    must("restamp_nodes_skipped_total_", "AC2", impl)
    must("lazy_only", "AC2", impl)
    must("use_incremental = true", "AC2", impl)
    must("silent torn generation", "AC2", impl)

    # AC3 metrics
    must("restamp-budget", "AC3", stdlib)
    must("restamp-budget-exceeded-total", "AC3", stdlib)
    must("restamp-nodes-skipped-total", "AC3", stdlib)
    must("restamp-last-budget-exceeded", "AC3", stdlib)
    must("schema-2934", "AC3", stdlib)
    must("schema-2934", "AC3 hold-stats", obs)
    must("restamp-budget-exceeded-total", "AC3 hold-stats", obs)

    # AC4 default unlimited
    must("0 = unlimited", "AC4", restamp)
    must("default Soft", "AC4", restamp)

    # AC5 source
    must("Issue #2934", "AC5 emb", emb)
    must("Issue #2934", "AC5 eix", eix)
    must("Issue #2934", "AC5 ast", astx)
    must("Issue #2934", "AC5 restamp", restamp)
    must("ac2934_1_budget_soft_degrade", "AC5 test", test)
    must("check_restamp_budget_2934", "AC5", build)

    # AC6
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2934-*")):
            fails.append(f"AC6: docs/design/{f.name} present")
    if (ROOT / "tests" / "core" / "test_issue_2934.cpp").is_file():
        fails.append("AC6: invent test_issue_2934.cpp")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2934 restamp budget — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

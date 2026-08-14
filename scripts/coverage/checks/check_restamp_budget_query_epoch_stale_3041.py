#!/usr/bin/env python3
"""Issue #3041: restamp budget exceed forces QueryEpoch stale (production).

On AURA_RESTAMP_BUDGET_NODES exceed under production:
  1. lazy-align still runs (is_valid consistent)
  2. active QueryEpoch (if any) is marked stale
  3. restamp-budget-query-epoch-stale-total + schema-3041 on
     query:query-epoch-stats / stable-ref-stats
Soft / unlimited: metric-only / zero extra QueryEpoch stores.

Contract:
  AC1 production + budget hit → QueryEpoch stale + counter
  AC2 lazy-align still runs
  AC3 Soft / Quiet unlimited zero extra
  AC4 extend restamp + isolation tests; schema additive
  AC5 this linter + 2934 linter lineage

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

    epoch = _read("src/core/workspace_epoch.hh")
    restamp = _read("src/core/flatast_restamp.hh")
    impl = _read("src/core/ast_impl.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qfile = _read("src/compiler/evaluator_primitives_query.cpp")
    gen = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    q = read_query_prims() + qfile
    sla = _read("tests/core/test_restamp_sla_observability.cpp")
    iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    lint2934 = _read("scripts/coverage/checks/check_restamp_budget_2934.py")
    build = _read("build.py")

    must("kRestampBudgetQueryEpochStaleIssue = 3041", "AC1", epoch)
    must("force_query_epoch_stale_from_restamp_budget", "AC1", epoch)
    must("g_query_epoch_forced_stale", "AC1", epoch)
    must("g_restamp_budget_query_epoch_stale_total", "AC1", epoch)
    must("force_query_epoch_stale_from_restamp_budget", "AC1 unified", fm)
    must("production", "AC1 production gate", fm)
    must("ac3041_1_production_budget_forces_query_epoch_stale", "AC1 test", sla)
    must("ac3041_1_query_epoch_forced_stale", "AC1 isolation", iso)

    must("Issue #3041", "AC2 impl", impl)
    must("lazy-align still runs", "AC2 impl", impl)
    must("restamp_lazy_align_enabled", "AC2", impl)
    must("ac3041_2_lazy_align_still_runs", "AC2 test", sla)
    must("kRestampBudgetQueryEpochStaleIssue = 3041", "AC2 restamp", restamp)

    must("ac3041_3_soft_unlimited_zero_extra", "AC3", sla)
    must("Soft stays metric-only", "AC3 impl", impl)
    must("if (production)", "AC3 no Soft force", fm)

    must("schema-3041", "AC4 query-epoch-stats", qws)
    must("restamp-budget-query-epoch-stale-total", "AC4 query-epoch-stats", qws)
    must("query-epoch-forced-stale", "AC4 query-epoch-stats", qws)
    must("schema-3041", "AC4 stable-ref-stats", q)
    must("schema-3041", "AC4 generation-stats", gen)
    must("schema-3041", "AC4 hold-stats", obs)
    must("schema-2934", "AC4 lineage 2934", gen)
    must("ac3041_4_schema_and_linter", "AC4 sla", sla)
    must("#3041", "AC4 isolation", iso)

    must("check_restamp_budget_query_epoch_stale_3041", "AC5", build)
    must("3041", "AC5 2934 linter", lint2934)
    if (ROOT / "tests" / "core" / "test_issue_3041.cpp").is_file():
        fails.append("AC5: test_issue_3041.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3041.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3041.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3041-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3041 restamp-budget QueryEpoch stale — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3075: production defaults arm QueryEpoch strict.

Residual of #3041 / #3037 / #3000: g_query_epoch_strict defaulted false
so production Agents could re-query green after mutate / restamp-lag
unless they set AURA_QUERY_EPOCH_STRICT=1.

Contract (one row per AC):
  AC1 apply_production_audit_defaults sets QueryEpoch strict;
      apply_dev / Soft leaves it off.
  AC2 restamp-budget force under production → finish_query_epoch false
      (query-epoch-stale). Unified restamp already calls the force helper.
  AC3 Soft / unlimited: no extra atomics on query_epoch_strict (does not
      OR production_defaults_active).
  AC4 Additive schema-3075 / query-epoch-production-strict-wired;
      2192 / 2933 / 3041 preserved.
  AC5 Extend test_query_epoch_contract + test_restamp_sla_observability;
      this linter; no invent test_issue_3075.cpp; no docs/design/.

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

    epoch = _read("src/core/workspace_epoch.hh")
    audit = _read("src/compiler/typed_mutation_audit.h")
    sec = _read("src/compiler/security_defaults.hh")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qec = _read("tests/compiler/test_query_epoch_contract.cpp")
    sla = _read("tests/core/test_restamp_sla_observability.cpp")
    build = _read("build.py")

    must("kQueryEpochProductionStrictIssue = 3075", "AC1 stamp", epoch)
    must("Issue #3075", "AC1 epoch", epoch)
    must("set_query_epoch_strict(true)", "AC1 apply_production", audit)
    must("set_query_epoch_strict(false)", "AC1 apply_dev", audit)
    must("Issue #3075", "AC1 audit", audit)
    must("set_query_epoch_strict(true)", "AC1 sampled production face", sec)
    must("set_query_epoch_strict(false)", "AC1 audit-off face", sec)

    must("force_query_epoch_stale_from_restamp_budget", "AC2 unified", fm)
    must("if (production)", "AC2 production gate", fm)
    must("ac3075_1_production_strict_finish_stale", "AC2 sla", sla)
    must("finish_query_epoch", "AC2 sla finish", sla)
    must("force_query_epoch_stale_from_restamp_budget", "AC2 contract", qec)
    must("query:result-fresh?", "AC2 Agent QueryResult", qec)
    must("finish_query_epoch", "AC2 finish canary", qec)

    must("do NOT OR production_defaults_active", "AC3 no extra load", epoch)
    must("ac3075_3_soft_unlimited_no_extra", "AC3 sla", sla)
    # query_epoch_strict body must stay a single acquire (not g_query_epoch_strict).
    qes = epoch.split("bool query_epoch_strict() noexcept")
    if len(qes) > 1 and "production_defaults_active()" in qes[1][:400]:
        fails.append("AC3: query_epoch_strict ORs production_defaults_active")

    must("schema-3075", "AC4 query-epoch-stats", qws)
    must("query-epoch-production-strict-wired", "AC4 wired", qws)
    must("schema-2192", "AC4 lineage 2192", qws)
    must("schema-2933", "AC4 lineage 2933", qws)
    must("schema-3041", "AC4 lineage 3041", qws)
    must("ac3075_4_schema_and_linter", "AC4 sla", sla)

    must("check_query_epoch_production_strict_3075", "AC5 build.py", build)
    must("#3075 AC1", "AC5 contract test", qec)
    must("ac3075_1_production_strict_finish_stale", "AC5 sla test", sla)
    if (ROOT / "tests" / "core" / "test_issue_3075.cpp").is_file():
        fails.append("AC5: test_issue_3075.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3075.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3075.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3075-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3075 production QueryEpoch strict — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3039: ScopedParallel overlap hard-rejects under production.

Successor of #2990 AC3 for production/Hard: overlapping regions must
return AdmissionRejected region-overlap (no silent SingleWriter degrade).
Soft / sandbox=off stays observe-only (existing overlap-reject-total).

Contract:
  AC1 production + ScopedParallel + overlap → hard reject, no fallback
  AC2 production + ScopedParallel + disjoint → RegionExclusive proceeds
  AC3 Soft / AURA_SANDBOX=off → zero extra hard-reject stores; metric only
  AC4 counter + posture keys (schema-3039 / wired)
  AC5 extend test_workspace_region_concurrency (#81967)
  AC6 this linter + 2990 linter updated

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

    hh = _read("src/compiler/workspace_concurrent_policy.hh")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    bud = _read("src/compiler/mutation_hold_budget.h")
    qws = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qh = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    q = read_query_prims() + qws
    t = _read("tests/compiler/test_workspace_region_concurrency.cpp")
    lint2990 = _read("scripts/coverage/checks/check_workspace_concurrent_policy_2990.py")
    build = _read("build.py")

    must("kWorkspaceScopedParallelOverlapHardIssue = 3039", "AC1", hh)
    must("Issue #3039", "AC1 policy", hh)
    must("g_mutation_region_overlap_hard_reject_total", "AC1", mb)
    must("AdmissionRejected: region-overlap", "AC1", mb)
    must("No GlobalExclusive fallback", "AC1", mb)
    must("ac3039_1_production_overlap_hard_reject", "AC1", t)

    must("ac3039_2_production_disjoint_proceeds", "AC2", t)
    must("bump_scoped_parallel_admit", "AC2", mb)

    must("ac3039_3_soft_observe_only", "AC3", t)
    must("does NOT bump hard-reject", "AC3", mb)
    must("g_mutation_region_overlap_reject_total", "AC3 metric", mb)

    must("g_mutation_region_overlap_hard_reject_total{0}", "AC4", bud)
    must("kMutationRegionOverlapHardRejectIssue = 3039", "AC4", bud)
    must_key("schema-3039", "AC4 stats", q)
    must_key("mutation-region-overlap-hard-reject-total", "AC4 stats", q)
    must_key("region-overlap-hard-reject-wired", "AC4 stats", q)
    must("schema-3039", "AC4 health", qh)
    must("ac3039_4_schema", "AC4", t)
    must("schema-2990", "AC4 lineage", q)

    must("ac3039_5_linter_and_suite", "AC5", t)
    must("ac2990_3_overlap_fallback", "AC5 #2990 kept", t)
    must("check_scoped_parallel_overlap_hard_reject_3039", "AC6", build)
    must("3039", "AC6 2990 linter", lint2990)
    if (ROOT / "tests" / "compiler" / "test_issue_3039.cpp").is_file():
        fails.append("AC5: test_issue_3039.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3039-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3039 ScopedParallel overlap hard-reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3007: Production residual identity CastOp in hot / post-mutate IR.

After solve_delta + CoercionMap rebuild, Production full-fn DeadCoercion
sweeps identity CastOps that #2556 cone-skip would leave. Soft keeps
density-policy / cone-external CastOps. Bidirectional covers mutate sites.

Contract:
  AC1 Production sweep elides cone-external identity CastOps
  AC2 Soft keeps cone-skip; no residual reject
  AC3 schema-3007 + lineage #2556; CoercionMap + run_coercion_elim wired
  AC4 extend test_dead_coercion_dirty_cone; linter; no docs/design/; no test_issue_3007

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    opt = _read("src/compiler/optimization_passes.ixx")
    svc = _read("src/compiler/service.ixx")
    ev = _read("src/compiler/evaluator_typecheck.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")

    # AC1
    must("kDeadCoercionHotResidualIssue", "AC1", opt)
    must("sweep_production_hot_residual_castops", "AC1", opt)
    must("count_identity_castops", "AC1", opt)
    must("ac3007_1_production_sweeps_cone_external", "AC1", t)

    # AC2
    must("Soft keeps #2556", "AC2", opt)
    must("ac3007_2_soft_keeps_cone_skip", "AC2", t)

    # AC3
    must("schema-3007", "AC3", q)
    must("hot-residual-sweep-total", "AC3", q)
    must("hot-residual-reject-total", "AC3", q)
    must("schema-2556", "AC3 lineage", q)
    must("sweep_production_hot_residual_castops", "AC3", svc)
    must("#3007", "AC3", ev)
    must("set_bidirectional_mode(true)", "AC3", ev)
    must("ac3007_3_schema_and_source", "AC3", t)

    # AC4
    must("check_dead_coercion_hot_residual_3007", "AC4", build)
    must("cmd_dead_coercion_hot_residual_3007_coverage", "AC4", build)
    must("ac3007_4_linter_no_design", "AC4", t)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3007*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_3007.cpp").is_file():
        fails.append("tests/compiler/test_issue_3007.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3007 Production hot residual CastOp sweep — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

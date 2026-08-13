#!/usr/bin/env python3
"""Issue #2993: type-check metrics tier (minimal default).

Hot consistent_unify / solve_delta fetch_adds gated unless Full.
Conflict / timeout / blame-missing / dynamic-degrade stay on.

Contract:
  AC1 Minimal default skips consistent_unify_total
  AC2 Full restores counters
  AC3 Dynamic-degrade still counted in Minimal
  AC4 AURA_TYPECHECK_METRICS + type:set-typecheck-metrics-tier + schema-2993
  AC5 extend existing suite; no docs/design / invent

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

    tix = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    compile_p = _read("src/compiler/evaluator_primitives_compile.cpp")
    svc = _read("src/compiler/service.ixx")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_epoch_filter.cpp")
    tb = _read("tests/compiler/test_typechecker_incremental_batch.cpp")
    tir = _read("tests/compiler/test_ir.cpp")
    build = _read("build.py")

    must("TypecheckMetricsTier", "AC1", tix)
    must("AURA_TYPECHECK_METRICS", "AC1", tix)
    must("metrics_full()", "AC1", tix)
    must("metrics_full()", "AC1 unify", impl)
    must("consistent_unify_total", "AC1", impl)
    must("ac2993_1_minimal_skips_unify_total", "AC1", t)

    must("TypecheckMetricsTier::Full", "AC2", tix)
    must("ac2993_2_full_restores", "AC2", t)
    must("#2993 Full restores unify_total", "AC2", tir)

    must("dynamic_degrade_with_blame_total", "AC3 keep", impl)
    must("ac2993_3_degrade_kept", "AC3", t)

    must("type:set-typecheck-metrics-tier", "AC4", compile_p)
    must("set_typecheck_metrics_tier", "AC4", svc)
    must_key("schema-2993", "AC4 query", q)
    must_key("typecheck-metrics-tier", "AC4 query", q)
    must("typecheck_metrics_tier", "AC4", met)
    must("ac2993_4_schema", "AC4", t)
    must("Performance notes", "AC4 docs", tix)

    must("ac2993_5_stress_minimal_zero", "AC5", t)
    must("ac2993_metrics_tier", "AC5 batch", tb)
    must("check_typecheck_metrics_tier_2993", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2993.cpp").is_file():
        fails.append("AC5: test_issue_2993.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2993-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2993 typecheck metrics tier — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2913: solve_delta locality SLO + escalate under production.

Contract:
  AC1 Production / Full + residual under-constrain after solve_delta →
      escalate full (or reject); Soft observes only
  AC2 Fully local SOLVED → no escalate, zero extra
  AC3 After escalate: production_escalated + residual cleared on SOLVED
  AC4 Additive schema-2913 + counters; preserve #1871/#2277/#2900
  AC5 Source-cite + extend test_solve_delta_unresolved_export; no docs/design/
  AC6 Soft vs production table in comments; solve_delta wrapper wired
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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    q = read_query_prims()
    metrics = _read("src/compiler/observability_metrics.h")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    # AC1: escalate API + Soft observe / production escalate paths
    must("escalate_locality_slo_if_production", "AC1", ixx)
    must("escalate_locality_slo_if_production", "AC1", impl)
    must("solve_delta_locality_escalate_total", "AC1", impl)
    must("solve_delta_locality_slo_observe_total", "AC1", impl)
    must("production_defaults_active", "AC1", impl)
    must("AuditStrategy::Full", "AC1", impl)

    # AC2: quiet path (no residual → return prior)
    must("last_locality_pruned_ == 0", "AC2", impl)
    must("dirty_count_ == 0", "AC2", impl)

    # AC3: production_escalated + residual clear on SOLVED escalate
    must("production_escalated_ = true", "AC3", impl)
    must("last_locality_pruned_ = 0", "AC3", impl)

    # AC4: schema + counters; preserve prior
    must("schema-2913", "AC4", q)
    must("issue-2913", "AC4", q)
    must("solve-delta-locality-escalate-total", "AC4", q)
    must("solve-delta-locality-slo-observe-total", "AC4", q)
    must("solve-delta-locality-reject-total", "AC4", q)
    must("solve-delta-locality-slo-wired", "AC4", q)
    must("solve-delta-locality-hits", "AC4", q)
    must("schema-2277", "AC4", q)
    must("schema-2900", "AC4", q)
    must("solve_delta_locality_escalate_total", "AC4", aud)
    must("solve_delta_locality_escalate_total", "AC4", metrics)

    # AC5: suite + linter + no docs
    must("ac2913_1_production_escalate", "AC5", t)
    must("ac2913_2_soft_observe_and_quiet", "AC5", t)
    must("ac2913_3_commit_readiness_after_escalate", "AC5", t)
    must("ac2913_4_additive_schema", "AC5", t)
    must("ac2913_5_source_cite", "AC5", t)
    must("check_solve_delta_locality_slo_2913", "AC5", build)
    must("2913", "AC5", ixx)
    must("2913", "AC5", impl)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2913-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2913.cpp").is_file():
        fails.append("tests/compiler/test_issue_2913.cpp present (forbidden #81967)")

    # AC6: Soft vs production table + wrapper call
    if "Soft + residual" not in impl and "Soft vs production" not in impl:
        fails.append("AC6: Soft vs production table missing in impl comments")
    must("escalate_locality_slo_if_production(result", "AC6", impl)
    must("last_locality_pruned_ = pruned", "AC6", impl)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2913 solve_delta locality SLO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

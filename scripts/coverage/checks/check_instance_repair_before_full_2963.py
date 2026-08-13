#!/usr/bin/env python3
"""Issue #2963: production prefer instance-repair before full-solve on TIMEOUT.

Residual of #2900: production defaults prefer_instance_repair_before_full;
on delta TIMEOUT repair local dirty + pending_full_solve_roots_ first;
only residual → full-solve escalate. Soft quiet zero cost; never ship
TIMEOUT / half-solved under production.

Contract:
  AC1 Production default prefer=true; dirty TIMEOUT → repair once; SOLVED
      allows; residual → full escalate; never TIMEOUT ship under production
  AC2 Soft quiet: no forced repair walk
  AC3 Additive delta-instance-repair-* + schema-2963; preserve #2900/#2277
  AC4 Zero cost when no TIMEOUT / no dirty+roots
  AC5 Extend test_solve_delta_unresolved_export; type_checker + tma cites;
      no docs/design/*
  AC6 Linter + build.py gate; large CS + small dirty repair hit

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    # AC1
    must("#2963", "AC1", ixx)
    must("prefer_instance_repair_before_full = true", "AC1", ixx)
    must("try_instance_repair_before_full", "AC1", ixx)
    must("try_instance_repair_before_full", "AC1", impl)
    must("delta_instance_repair_total", "AC1", impl)
    must("delta_instance_repair_resolved_total", "AC1", impl)
    must("ac2963_1_production_repair_resolves", "AC1", t)

    # AC2
    must("#2963 AC2", "AC2", impl)
    must("ac2963_2_soft_quiet_zero_cost", "AC2", t)

    # AC3
    must("schema-2963", "AC3", q)
    must("delta-instance-repair-total", "AC3", q)
    must("delta-instance-repair-resolved-total", "AC3", q)
    must("delta-timeout-full-after-repair-total", "AC3", q)
    must("schema-2900", "AC3 lineage", q)
    must("schema-2277", "AC3 lineage", q)
    must("delta_instance_repair_total", "AC3", aud)

    # AC4
    must("zero cost", "AC4", impl)
    must("ac2963_3_quiet_no_timeout_zero", "AC4", t)

    # AC5
    must("kSolverBudgetInstanceRepairIssue", "AC5", ixx)
    must("check_instance_repair_before_full_2963", "AC5", build)
    must("ac2963_5_source_cite", "AC5", t)
    must("ac2963_6_large_cs_small_dirty_repair_hit", "AC5", t)

    # AC6
    must("cmd_instance_repair_before_full_2963_coverage", "AC6", build)
    must("instance-repair-before-full-2963", "AC6", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2963-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2963.cpp").is_file():
        fails.append("tests/compiler/test_issue_2963.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2963 instance-repair-before-full — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

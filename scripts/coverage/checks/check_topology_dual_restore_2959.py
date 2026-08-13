#!/usr/bin/env python3
"""Issue #2959: MutationBoundaryGuard abort dual topology restore (children_+parent_).

Contract:
  AC1 Guard abort paths use abort_restore_dual_topology under structural exclusive
  AC2 topology_dual_restore_total + inconsistency canary; schema-2959
  AC3 densify×steal soak lite dual-consistent (tests)
  AC4 Soft / unit green; no docs/design/*
  AC5 tests extend restore_children suite; no invent test file
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

    ast = _read("src/core/ast.ixx")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    metrics = _read("src/compiler/observability_metrics.h")
    test = _read("tests/core/test_restore_children_structural_lock.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2959", "AC1", ast)
    must("abort_restore_dual_topology", "AC1", ast)
    must("seal_dual_topology_restore_locked", "AC1", ast)
    must("verify_children_parent_topology_consistent", "AC1", ast)
    must("abort_restore_dual_topology", "AC1 Guard", emb)
    if emb.count("abort_restore_dual_topology") < 2:
        fails.append("AC1: Guard abort must call dual restore on ≥2 paths")
    # No split rollback+restore on main abort (prefer dual API).
    # Fine-rollback still allowed; main failure path should use dual API.
    must("Issue #2959", "AC1 emb", emb)

    # AC2
    must("topology_dual_restore_total_", "AC2", ast)
    must("topology_dual_restore_inconsistency_total_", "AC2", ast)
    must("topology_dual_restore_total", "AC2 metrics", metrics)
    must("schema-2959", "AC2", q)
    must("topology-dual-restore-total", "AC2", q)
    must("topology-dual-restore-inconsistency-total", "AC2", q)
    if "make_int(1502)" not in q and "schema-1502" not in q:
        fails.append("AC2: #1502 lineage schema missing")

    # AC3–AC5
    must("abort_restore_dual_topology", "AC3 test", test)
    must("2959 AC3", "AC3 soak", test)
    must("check_topology_dual_restore_2959", "AC5", build)
    must("2959 AC5", "AC5 test", test)
    if (ROOT / "tests" / "core" / "test_issue_2959.cpp").is_file():
        fails.append("AC5: test_issue_2959.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2959-*"):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2959 topology dual restore")
    return 0


if __name__ == "__main__":
    sys.exit(main())

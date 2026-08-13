#!/usr/bin/env python3
"""Issue #2985: production mutation-concurrency-health auto-reject admit.

Contract:
  AC1 production + hard force_reason → try_acquire rejects; Soft observe
  AC2 production + health_bp < budget → reject GlobalExclusive
  AC3 happy path no extra admit stores
  AC4 additive schema-2985; compute score non-regressing
  AC5 inject deny; clear → admit resumes
  AC6 source-cite try_acquire + header; no docs/design/
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

    hh = _read("src/compiler/mutation_concurrency_health.hh")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_mutation_concurrency_health.cpp")
    build = _read("build.py")

    must("Issue #2985", "AC1", hh)
    must("Issue #2985", "AC1", mb)
    must("maybe_reject_mutation_concurrency_health", "AC1", mb)
    must("set_mutation_concurrency_health_soft_for_test", "AC1", hh)
    must("ac2985_1_prod_hard_reason_rejects", "AC1", t)

    must("h.health_bp < h.health_budget_bp", "AC2", hh)
    must("ac2985_2_under_budget_rejects", "AC2", t)

    must("Happy path", "AC3", hh)
    must("ac2985_3_happy_zero_extra", "AC3", t)

    must("schema-2985", "AC5", q)
    must("mutation-concurrency-health-reject-total", "AC5", q)
    must("schema-2379", "AC5", q)
    must("ac2985_4_additive_query", "AC5", t)

    must("ac2985_5_inject_clear_resumes", "AC5", t)
    must("set_mutation_concurrency_health_admit_snapshot_for_test", "AC5", hh)

    must("ac2985_6_source_and_linter", "AC6", t)
    must("check_mutation_concurrency_health_admit_2985", "AC6", build)
    must("compute_mutation_concurrency_health", "AC6", hh)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2985-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2985.cpp").is_file():
        fails.append("tests/compiler/test_issue_2985.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2985 concurrency-health admit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

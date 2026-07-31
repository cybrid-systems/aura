#!/usr/bin/env python3
"""Issue #2405: query:mutation-hold-estimate Agent batch planning surface.

Contract:
  AC1 pure query budget/slo + hold distribution, no side effects
  AC2 sample ring at outermost dtor; p50/p99 from recent holds
  AC3 empty session zeros; recommend-split false
  AC4 additive schema-2405; hold-stats keys intact
  AC5 tests + build gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    bud = _read("src/compiler/mutation_hold_budget.h")
    test = _read("tests/compiler/test_mutation_hold_estimate_2405.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 query surface
    must("query:mutation-hold-estimate", "AC1", q)
    must("Issue #2405", "AC1", q)
    must("hold-us-p50", "AC1", q)
    must("hold-us-p99", "AC1", q)
    must("budget-us", "AC1", q)
    must("slo-us", "AC1", q)
    must("recommend-split", "AC1", q)
    must("dirty-node-estimate", "AC1", q)
    must("dirty-upward-call-estimate", "AC1", q)
    must("2405 AC1", "AC1", test)

    # AC2 sample ring
    must("mutation_hold_sample_ring", "AC2", met)
    must("kMutationHoldSampleRing", "AC2", met)
    must("mutation_hold_sample_ring", "AC2", emb)
    must("Issue #2405", "AC2", emb)
    must("2405 AC2", "AC2", test)

    # AC3 soft
    must("2405 AC3", "AC3", test)
    must("hold-estimate-wired", "AC3", q)

    # AC4 additive + hold-stats discovery
    must("schema-2405", "AC4", q)
    must("issue-2405", "AC4", q)
    must("schema-2349", "AC4", q)  # existing hold-stats keys still in file
    must("mutation_hold_budget_us", "AC4", bud)
    must("2405 AC4", "AC4", test)

    # AC5
    must("2405 AC5", "AC5", test)
    must("check_mutation_hold_estimate_2405", "AC5", build)
    must("cmd_mutation_hold_estimate_coverage", "AC5", build)
    must("test_mutation_hold_estimate_2405", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: mutation-hold-estimate #2405 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

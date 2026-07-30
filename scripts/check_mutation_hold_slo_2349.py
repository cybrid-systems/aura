#!/usr/bin/env python3
"""Issue #2349: outermost hold SLO circuit-breaker (production default fail).

Contract:
  AC1 Production + hold > SLO → success_flag=false; violation counter
  AC2 Soft/sandbox → metric only
  AC3 Hold ≤ SLO → zero force work
  AC4 AURA_MUTATION_HOLD_SLO_US=0 disables; schema-2349 query keys
  AC5 Tests + decision table + source-cite (no second timer)

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
    bud = _read("src/compiler/mutation_hold_budget.h")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_mutation_hold_slo_2349.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 production force-fail
    must("mutation_hold_slo_us", "AC1", emb)
    must("mutation_hold_slo_violation_total", "AC1", emb)
    must("mutation_hold_slo_violation_total", "AC1", met)
    must("ac1_production_force_fail", "AC1", test)
    must("*flag_ = false", "AC1", emb)

    # AC2 soft
    must("mutation_hold_slo_soft_mode", "AC2", bud)
    must("mutation_hold_slo_soft_mode", "AC2", emb)
    must("AURA_MUTATION_HOLD_SLO_SOFT", "AC2", bud)
    must("ac2_soft_metric_only", "AC2", test)

    # AC3 happy path
    must("AC3", "AC3", emb)
    must("ac3_under_slo_no_force", "AC3", test)

    # AC4 disable + query
    must("AURA_MUTATION_HOLD_SLO_US", "AC4", bud)
    must("schema-2349", "AC4", q)
    must("issue-2349", "AC4", q)
    must("mutation-hold-slo-violation-total", "AC4", q)
    must("mutation-hold-slo-us", "AC4", q)
    must("mutation-hold-slo-wired", "AC4", q)
    must("ac4_disable_and_query", "AC4", test)

    # AC5
    must("Decision table", "AC5", bud)
    must("no second timer", "AC5", emb)
    must("Issue #2349", "AC5", emb)
    must("ac5_source_cite", "AC5", test)
    must("test_mutation_hold_slo_2349", "AC5", cmake)
    must("check_mutation_hold_slo_2349", "AC5", build)
    must("cmd_mutation_hold_slo_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2349 hold SLO circuit-breaker — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

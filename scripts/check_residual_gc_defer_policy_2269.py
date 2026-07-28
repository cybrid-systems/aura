#!/usr/bin/env python3
"""check_residual_gc_defer_policy_2269.py — Issue #2269 source gate.

  AC1: AURA_RESIDUAL_DEFER_POLICY env var + ResidualPolicy {Soft, Clear, Hard}
  AC2: Production default is Clear (not soft-only)
  AC3: Zero-cost success path (single relaxed load of reasons)
  AC4: 2 new counters + 5 new query keys + schema-2269/issue-2269 lineage
  AC5: Test extension (tests/compiler/test_residual_gc_defer_assert_2211.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVAL_MB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
PRIM_Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_residual_gc_defer_assert_2211.cpp"


def main() -> int:
    failures: list[str] = []

    mb = EVAL_MB.read_text(encoding="utf-8", errors="replace")
    metrics = METRICS.read_text(encoding="utf-8", errors="replace")
    q = PRIM_Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: env-driven policy + ResidualPolicy enum
    must("AURA_RESIDUAL_DEFER_POLICY", "AC1", mb)
    must("AURA_HARD_RESIDUAL_DEFER", "AC1", mb)
    must("ResidualPolicy", "AC1", mb)
    must("policy == ResidualPolicy::Hard", "AC1", mb)
    must("policy == ResidualPolicy::Clear", "AC1", mb)

    # AC2: production default is Clear (not soft-only)
    must("ResidualPolicy::Clear", "AC2", mb)
    must("dev_off", "AC2", mb)

    # AC3: zero-cost success path (single relaxed load)
    must(
        "const auto residual = aura::gc_hooks::defer_reasons_snapshot()",
        "AC3",
        mb,
    )
    must("if (residual != 0)", "AC3", mb)

    # AC4: counter fields + query keys + schema-2269 lineage
    must(
        "mutation_boundary_residual_defer_forced_clear_total{0}",
        "AC4",
        metrics,
    )
    must(
        "mutation_boundary_residual_defer_hard_fail_total{0}",
        "AC4",
        metrics,
    )
    must("residual-defer-forced-clear-total", "AC4", q)
    must("residual-defer-hard-fail-total", "AC4", q)
    must("residual-defer-policy", "AC4", q)
    must("residual-defer-policy-wired", "AC4", q)
    must("schema-2269", "AC4", q)
    must("issue-2269", "AC4", q)

    # AC5: test extension (ac2269_residual_defer_policy + 5-AC rows + runtime smoke)
    must("void ac2269_residual_defer_policy", "AC5", test)
    must("ac2269_residual_defer_policy(cs)", "AC5", test)
    must("AC #2269: residual defer policy (soft | clear | hard)", "AC5", test)
    must(
        "AC5-soft: forced-clear counter unchanged under soft policy",
        "AC5",
        test,
    )
    must(
        "AC5-clear: forced-clear counter bumped under production-default Clear",
        "AC5",
        test,
    )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

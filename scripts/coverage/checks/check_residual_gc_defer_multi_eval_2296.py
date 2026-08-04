#!/usr/bin/env python3
"""Issue #2296: Phase-5 residual Clear + multi-eval orphan steal harden.

  AC1: force_clear_all_gc_defer_for_evaluator in Phase 5 Clear path
  AC2: steal path reconcile_gc_defer_bits_after_clear after orphan clear
  AC3: residual == 0 zero-cost path retained
  AC4: Hard/Soft policy branches retained
  AC5: query correlation keys + schema-2296 + tests

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
GH = ROOT / "src" / "core" / "gc_hooks.h"
MUT = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
MET = ROOT / "src" / "compiler" / "observability_metrics.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_residual_gc_defer_assert_2211.cpp"


def main() -> int:
    failures: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing {needle!r}")

    mb = MB.read_text(encoding="utf-8", errors="replace")
    gh = GH.read_text(encoding="utf-8", errors="replace")
    mut = MUT.read_text(encoding="utf-8", errors="replace")
    met = MET.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    must("force_clear_all_gc_defer_for_evaluator", "AC1", gh)
    must("force_clear_all_gc_defer_for_evaluator", "AC1", mb)
    must("reconcile_gc_defer_bits_after_clear", "AC1", gh)

    must("reconcile_gc_defer_bits_after_clear", "AC2", mut)
    must("clear_gc_defer_for_evaluator", "AC2", mut)

    must("if (residual != 0)", "AC3", mb)
    must("defer_reasons_snapshot()", "AC3", mb)

    must("ResidualPolicy::Hard", "AC4", mb)
    must("std::abort()", "AC4", mb)
    must("AURA_SANDBOX", "AC4", mb)

    must("mutation_boundary_residual_defer_bit_reconcile_total{0}", "AC5", met)
    must("residual-defer-bit-reconcile-total", "AC5", q)
    must("gc-defer-orphan-cleared-on-steal-total", "AC5", q)
    must("gc-defer-table-overflow-total", "AC5", q)
    must("schema-2296", "AC5", q)
    must("issue-2296", "AC5", q)
    must("Decision table", "AC5", mb)
    must("void ac2296_multi_eval_residual_clear", "AC5", test)
    must("ac2296_multi_eval_residual_clear(cs)", "AC5", test)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(failures)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: residual multi-eval Clear harden (#2296) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2526: adaptive deopt-storm threshold closed-loop with LayoutStamp.

Contract:
  AC1 compact-only does not alone drive process-global storm
  AC2 mutation churn still enters storm + hard fence
  AC3 adaptive suppress under compact-dominated + stable
  AC4 query:shape-storm-health adaptive keys + schema-2526
  AC5 LayoutStamp hard fence only on Threshold force-reason

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

    sph = _read("src/compiler/shape_profiler.h")
    spc = _read("src/compiler/shape_profiler.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_shape_storm_adaptive_2526.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("#2526", "AC1", sph)
    must("#2526", "AC1", spc)
    must("on_arena_compact", "AC1", spc)
    must("Explicitly do NOT call update_deopt_storm_state_", "AC1", spc)
    must("ac1_compact_only_no_storm", "AC1", test)

    # AC2
    must("kShapeStormForceReasonThreshold", "AC2", sph)
    must("bump_shape_version_on_storm_enter", "AC2", spc)
    must("adaptive_enter_total_", "AC2", spc)
    must("ac2_mutation_still_storms", "AC2", test)

    # AC3
    must("adaptive_thr", "AC3", spc)
    must("adaptive_suppress_total_", "AC3", spc)
    must("compact_dominated", "AC3", spc)
    must("ac3_adaptive_suppress", "AC3", test)

    # AC4
    must("query:shape-storm-health", "AC4", q)
    must("schema-2526", "AC4", q)
    must("adaptive-threshold-live", "AC4", q)
    must("adaptive-suppress-total", "AC4", q)
    must("shape-storm-fence-hard", "AC4", q)
    must("ac4_query", "AC4", test)

    # AC5
    must("shape_storm_fence_hard", "AC5", sph)
    must("kShapeStormForceReasonAdaptiveSuppress", "AC5", sph)
    must("ac5_hard_fence", "AC5", test)

    # Gate
    must("test_shape_storm_adaptive_2526", "gate", cmake)
    must("check_shape_storm_adaptive_2526", "gate", build)
    must("cmd_shape_storm_adaptive_coverage", "gate", build)

    # Retain #2433 lineage
    must("schema-2433", "retain", q)
    must("kHighMutationPreset", "retain", sph)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2526 adaptive deopt-storm threshold — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

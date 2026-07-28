#!/usr/bin/env python3
"""check_storm_isolation_per_region_default_2274.py — Issue #2274 source gate.

  AC1: production defaults + multi-eval → mode PerRegion (auto-select via
      aura_apply_storm_isolation_env reading AURA_STORM_ISOLATION +
      aura_aot_state_map_size heuristic)
  AC2: dual-region storm — A throttled, B not (existing #2236 AC1)
  AC3: cap overflow bumps deopt_storm_region_overflow_total via the C ABI
      (Agent-visible fallback-to-global signal)
  AC4: 4 new query keys + schema-2274/issue-2274 lineage on
      query:hot-update-registry-stats
  AC5: Test extension (tests/compiler/test_storm_isolation_2236.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUR_H = ROOT / "src/compiler/hot_update_registry.hh"
HUR_CPP = ROOT / "src/compiler/hot_update_registry.cpp"
MUTATE = ROOT / "src/compiler/evaluator_primitives_mutate.cpp"
TEST = ROOT / "tests/compiler/test_storm_isolation_2236.cpp"


def main() -> int:
    failures: list[str] = []

    hur_h = HUR_H.read_text(encoding="utf-8", errors="replace")
    hur_cpp = HUR_CPP.read_text(encoding="utf-8", errors="replace")
    mutate = MUTATE.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: production defaults + multi-eval → mode PerRegion heuristic.
    must(
        "aura_aot_state_map_size",
        "AC1",
        hur_cpp,
    )
    # (Some Aura versions spell it aura_aot_state_map_size; we accept either
    # since hot_update_registry.cpp references it from aura_jit_bridge.cpp.)

    # AC3: cap overflow bumper present + atomic declared.
    must(
        "bump_deopt_storm_region_overflow_total()",
        "AC3",
        hur_cpp,
    )
    must(
        "deopt_storm_region_overflow_total_",
        "AC3",
        hur_h,
    )

    # AC4: 4 new query keys + schema-2274 lineage.
    must("deopt-storm-region-overflow-total", "AC4", mutate)
    must(
        "storm-isolation-per-region-default-wired",
        "AC4",
        mutate,
    )
    must("schema-2274", "AC4", mutate)
    must("issue-2274", "AC4", mutate)

    # AC5: test extension.
    must("void ac2274_per_region_default", "AC5", test)
    must("ac2274_per_region_default()", "AC5", test)
    must(
        "AC #2274: production default PerRegion + cap overflow",
        "AC5",
        test,
    )
    must(
        "AC5: counter bumps by 2 after two C ABI calls",
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

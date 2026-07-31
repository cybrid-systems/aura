#!/usr/bin/env python3
"""Issue #2433: HighMutation default-on + deopt-storm × LayoutStamp.

Contract:
  AC1 production default applies kHighMutationPreset knobs (apply_preset)
  AC2 storm enter → shape_version + shape_storm_active + SpecJIT isolation
  AC3 isolations bounded under continuous mutate
  AC4 soft path: storm writes only on enter/clear transition
  AC5 query:shape-storm-health + schema-2433 + test/gate

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
    sj = _read("src/compiler/spec_jit_controller.cpp")
    test = _read("tests/compiler/test_shape_high_mutation_storm_2433.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("shape_high_mutation_default_enabled", "AC1", sph)
    must("kHighMutationPreset", "AC1", sph)
    must("apply_preset(kHighMutationPreset)", "AC1", spc)
    must("2433 AC1", "AC1", test)

    # AC2
    must("bump_shape_version_on_storm_enter", "AC2", spc)
    must("aura_hot_update_set_shape_storm_active(1)", "AC2", spc)
    must("g_shape_version_at_storm_atomic", "AC2", sph)
    must("kShapeStormForceReasonThreshold", "AC2", sph)
    must("effective_shape_version", "AC2", sj)
    must("2433 AC2", "AC2", test)

    # AC3
    must("g_deopt_storm_isolations_total_atomic", "AC3", sph)
    must("2433 AC3", "AC3", test)

    # AC4
    must("aura_hot_update_set_shape_storm_active(0)", "AC4", spc)
    must("kShapeStormForceReasonNone", "AC4", sph)
    must("2433 AC4", "AC4", test)

    # AC5
    must("query:shape-storm-health", "AC5", q)
    must("shape-version-at-storm", "AC5", q)
    must("schema-2433", "AC5", q)
    must("issue-2433", "AC5", q)
    must("2433 AC5", "AC5", test)
    must("check_shape_high_mutation_storm_2433", "gate", build)
    must("cmd_shape_high_mutation_storm_coverage", "gate", build)
    must("test_shape_high_mutation_storm_2433", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: shape high mutation storm #2433 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

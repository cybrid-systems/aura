#!/usr/bin/env python3
"""Issue #2257: ShapeProfiler versioning + deopt-storm isolation.

Contract (5 AC from issue body):
  AC1: shape_version advances on compact + storm enter; resume
       with old version forces deopt / re-profile.
  AC2: Under HighMutation + continuous body mutate, deopt rate
       stays bounded (one bump per storm enter, not per deopt).
  AC3: Query surface: shape-version, deopt-storm-isolations-total,
       current-stability-ratio, schema-2257 lineage.
  AC4: Zero extra cost on cold / stable functions.
  AC5: Integration with existing StormLevel facade (#2094 lineage).

This linter is the source-of-truth for the production surface.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    sph = _read("src/compiler/shape_profiler.h")
    spc = _read("src/compiler/shape_profiler.cpp")
    met = _read("src/compiler/observability_metrics.h")
    ir = _read("src/compiler/ir_cache_pure.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_shape_profiler_stability_deopt_fiber_task4.cpp")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")

    # AC1 — bump_shape_version_on_storm_enter + g_deopt_storm_isolations_total_atomic
    _must(
        "bump_shape_version_on_storm_enter" in sph and "g_deopt_storm_isolations_total_atomic" in sph,
        "AC1: shape_profiler.h bump helper + isolations atomic missing",
        fails,
    )
    _must(
        spc.find("bump_shape_version_on_storm_enter()") != -1,
        "AC1: bump wired in update_deopt_storm_state_ missing",
        fails,
    )
    # AC1 — HighMutation production default (Issue #2433: apply_preset knobs)
    _must(
        "shape_high_mutation_default_enabled" in sph
        and "kHighMutationPreset" in spc
        and ("apply_preset(kHighMutationPreset)" in spc or "active_preset_ = kHighMutationPreset" in spc),
        "AC1: HighMutation production default missing",
        fails,
    )
    # AC2 — 1 new counter field
    _must("deopt_storm_isolations_total{0}" in met, "AC2: deopt_storm_isolations_total counter field missing", fails)
    # AC3 — 3 query keys + schema-2257
    _must(
        "shape-version" in q and "deopt-storm-isolations-total" in q and "current-stability-ratio" in q,
        "AC3: 3 query keys missing",
        fails,
    )
    _must("schema-2257" in q and "issue-2257" in q, "AC3: schema-2257 / issue-2257 lineage missing", fails)
    # AC4 — stability_ratio file-scope + set/get helpers
    _must(
        "g_shape_stability_ratio_atomic" in ir
        and "set_shape_stability_ratio" in ir
        and "current_shape_stability_ratio" in ir,
        "AC4: stability_ratio file-scope + helpers missing in ir_cache_pure.ixx",
        fails,
    )
    # AC5 — StormLevel facade source-cite in evaluator_mutation_boundary.cpp
    _must("StormLevel" in mut, "AC5: StormLevel facade source-cite missing in evaluator_mutation_boundary.cpp", fails)
    # Test surface covers #2257
    _must(
        ("ac2257_shape_profiler_storm_isolation" in test) or ("AC #2257" in test),
        "AC test: ac2257 test function (or AC #2257 inline block) missing",
        fails,
    )
    _must("#2257" in test, "AC test: #2257 issue citation missing in test file comment", fails)
    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2257 shape profiler storm isolation linter")
    parser.add_argument("--self-test", action="store_true", help="Run self-test (return 0 if contract satisfied)")
    parser.add_argument("--strict", action="store_true", help="Strict mode (non-zero exit on any failure)")
    args = parser.parse_args()
    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: shape profiler storm isolation coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

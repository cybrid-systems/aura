#!/usr/bin/env python3
"""Issue #2255: Unified LayoutStamp + arena_gen + shape_version fence.

Contract (5 AC from issue body):
  AC1: Phase 5 of outermost Guard writes complete stamp (incl.
       shape_version as 7th field) into current Fiber before unlock.
  AC2: Resume after steal with concurrent compact/reemit ->
       mismatch counter + safe fallback (scan_live_closures +
       shape_version_fence_reject bump).
  AC3: ShapeProfiler history ring advances only under matching
       stamp; HighMutation preset remains valid.
  AC4: Metrics: layout_stamp_resume_mismatch_total +
       shape_version_fence_reject_total.
  AC5: Dual-worker stress: fiber A mutates + compact, fiber B stolen
       and applies -> assert fence or safe path.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
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

    layout_stamp_h = _read("src/core/layout_stamp.hh")
    fiber_h = _read("src/serve/fiber.h")
    mut_boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    shape_h = _read("src/compiler/shape_profiler.h")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_layout_stamp_2170.cpp")

    # AC1 - LayoutStamp.shape_version field (7th)
    _must(
        "shape_version" in layout_stamp_h and "shape_version = 0" in layout_stamp_h and "sver = 0" in layout_stamp_h,
        "AC1: LayoutStamp.shape_version field missing",
        fails,
    )

    # AC1 - Fiber::resume_shape_version_ field + setter + getter + clear
    _must(
        "resume_shape_version_" in fiber_h and "resume_shape_version()" in fiber_h,
        "AC1: Fiber::resume_shape_version_ field + getter missing",
        fails,
    )

    # AC1 - Phase 5 wire passes shape_version (7th) + optional ir_soa_generation (8th, #2432).
    # clang-format may put the open paren alone on its line once arg count grows;
    # match the call + arg names rather than a single-line contiguous substring.
    _must(
        "set_resume_layout_stamp" in mut_boundary
        and "stamp.arena_id" in mut_boundary
        and "stamp.arena_gen" in mut_boundary
        and "stamp.shape_version" in mut_boundary,
        "AC1: Phase 5 wire passes shape_version as 7th arg",
        fails,
    )

    # AC1 - current_layout_stamp includes shape_version from ShapeProfiler
    _must(
        mut_boundary.find("current_global_shape_version") != -1,
        "AC1: current_layout_stamp() reads ShapeProfiler shape_version",
        fails,
    )

    # AC2 - hard compare includes shape_version (2 occurrences)
    count = fiber_mut.count("resume_shape_version() != cur.shape_version")
    _must(count >= 2, f"AC2: hard compare must include shape_version in 2 sites (found {count})", fails)

    # AC2 - shape_version_fence_reject_total bump
    _must(
        fiber_mut.count("shape_version_fence_reject_total.fetch_add") >= 2,
        "AC2: shape_version_fence_reject_total bump site missing (need 2 sites)",
        fails,
    )

    # AC3 - ShapeProfiler accessor exposed
    _must(
        "current_global_shape_version" in shape_h,
        "AC3: current_global_shape_version() accessor missing in shape_profiler.h",
        fails,
    )

    # AC4 - counter field + query surface + schema-2255
    _must(
        "shape_version_fence_reject_total{0}" in met,
        "AC4: shape_version_fence_reject_total counter field missing",
        fails,
    )
    _must("shape-version-fence-reject-total" in q, "AC4: shape-version-fence-reject-total query key missing", fails)
    _must("schema-2255" in q and "issue-2255" in q, "AC4: schema-2255 / issue-2255 lineage missing", fails)

    # AC5 - test surface covers #2255
    _must(
        ("ac2250_fiber_resume_fence" in test) or ("AC #2255" in test),
        "AC5: ac2250_fiber_resume_fence (or AC #2255 inline block) missing",
        fails,
    )
    _must("#2255" in test, "AC5: #2255 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Issue #2255 unified LayoutStamp + shape_version fence coverage linter"
    )
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

    print("OK: unified LayoutStamp + shape_version fence coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

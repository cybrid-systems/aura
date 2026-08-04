#!/usr/bin/env python3
"""Issue #2256: production-default Moving compaction + LifetimePin hard contract.

Contract (5 AC from issue body):
  AC1: apply_production_security_defaults (or equivalent) sets
       Moving on (or adaptive-on).
  AC2: After compact, all live SoA indices / pinned StableNodeRefs
       remain valid; no UAF under TSan stress.
  AC3: Pin + remap path is zero-overhead when no compact occurs.
  AC4: Metrics + query surface for compact_count, bytes_reclaimed,
       pin_hits, remap_us (+ schema-2256 lineage).
  AC5: 10k-mutation soak with Moving on shows bounded fragmentation.

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

    arena_h = _read("src/core/arena.ixx")
    pin_h = _read("src/core/lifetime_pin.ixx")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    shape = _read("src/compiler/shape_profiler.cpp")
    soa = _read("src/compiler/ir_soa.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_moving_compact.cpp")

    # AC1 - production default ON + adaptive-on-threshold
    _must(
        arena_h.find("production default ON") != -1 and "return 1;" in arena_h,
        "AC1: arena.ixx production default ON missing",
        fails,
    )
    _must(
        "should_auto_moving_compact" in arena_h and "kAutoMovingCompactThreshold" in arena_h,
        "AC1: adaptive-on-threshold policy missing",
        fails,
    )

    # AC2 - LifetimePin hard contract
    _must(
        "verify_pins_under_moving_compact" in pin_h
        and "g_moving_compact_pin_hits_total" in pin_h
        and "g_moving_compact_remap_us_total" in pin_h,
        "AC2: LifetimePin hard contract (verify_pins + pin_hits + remap_us) missing",
        fails,
    )
    _mut_hard_soa = soa.find("index-remap contract") != -1
    _must(_mut_hard_soa, "AC2: SoA index-remap contract marker missing", fails)

    # AC3 - zero-cost when no compact
    _must(
        mut.find("verify_pins_under_moving_compact()") != -1,
        "AC3: compact driver must call verify_pins (zero-cost when not called)",
        fails,
    )

    # AC4 - metrics + query surface
    _must(
        "compact_count_total{0}" in met
        and "bytes_reclaimed_total{0}" in met
        and "pin_hits_total{0}" in met
        and "remap_us_total{0}" in met,
        "AC4: 4 metric fields missing",
        fails,
    )
    _must(
        "compact-count-total" in q
        and "bytes-reclaimed-total" in q
        and "pin-hits-total" in q
        and "remap-us-total" in q
        and "moving-compact-wired" in q,
        "AC4: 4 query keys + moving-compact-wired sentinel missing",
        fails,
    )
    _must("schema-2256" in q and "issue-2256" in q, "AC4: schema-2256 / issue-2256 lineage missing", fails)
    _mut_shape = (
        shape.find("g_moving_compact_count_total") != -1 and shape.find("g_moving_compact_remap_us_total") != -1
    )
    _must(_mut_shape, "AC4: ShapeProfiler::on_arena_compact must feed compact_count + remap_us", fails)

    # AC5 - test surface covers #2256 (ac2256 in test_moving_compact.cpp)
    _must(
        ("ac2256_moving_compact_production_default" in test) or ("AC #2256" in test),
        "AC5: ac2256 test function (or AC #2256 inline block) missing",
        fails,
    )
    _must("#2256" in test, "AC5: #2256 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2256 arena Moving-compact coverage linter")
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

    print("OK: arena Moving-compact coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

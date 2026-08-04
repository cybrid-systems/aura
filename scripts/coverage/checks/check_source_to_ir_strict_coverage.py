#!/usr/bin/env python3
"""Issue #2244: source_to_ir_map Strict-mode hard-fail + rebuild coverage.

Contract (5 AC from issue body):
  AC1: Injected desync -> rebuild runs + hard_fail_total increments
  AC2: Off / unit-test default remains soft
  AC3: Happy path (consistent map) zero extra cost beyond count_*
  AC4: Query surface (additive): source-to-ir-inconsistency-total +
       source-to-ir-hard-fail-total + schema-2244 lineage
  AC5: This script (contract rows for rebuild call sites + Strict gate
       + metric fields)

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

    pure = _read("src/compiler/ir_cache_pure.ixx")
    met = _read("src/compiler/observability_metrics.h")
    dirty = _read("src/compiler/service_dirty.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_source_to_ir_map_consistency_2045.cpp")

    # AC1/AC2/AC3 — helper in ir_cache_pure.ixx
    _must(
        "ensure_source_to_ir_or_rebuild" in pure,
        "AC1/AC3: ensure_source_to_ir_or_rebuild helper missing in ir_cache_pure.ixx",
        fails,
    )
    _must(
        "enum class SourceToIrStrictMode" in pure,
        "AC1/AC2: SourceToIrStrictMode enum missing in ir_cache_pure.ixx",
        fails,
    )
    _must(
        "struct EnsureSourceToIrResult" in pure,
        "AC1/AC3: EnsureSourceToIrResult struct missing in ir_cache_pure.ixx",
        fails,
    )
    _must("Strict" in pure and "Off" in pure, "AC2: SourceToIrStrictMode::Off + ::Strict missing", fails)
    _must(
        "if (r.was_consistent)" in pure and "return r" in pure,
        "AC3: zero-cost early return on consistent path missing",
        fails,
    )

    # AC1 — wire-up at invalidate_bridge_with_impact lambda
    _must("ensure_source_to_ir_or_rebuild" in dirty, "AC1: wire-up call missing in service_dirty.cpp", fails)
    _must("invalidate_bridge_with_impact" in dirty, "AC1: invalidate_bridge_with_impact lambda missing (sanity)", fails)
    _must(
        "g_source_to_ir_strict" in dirty,
        "AC1/AC2: atomic toggle g_source_to_ir_strict missing in service_dirty.cpp",
        fails,
    )
    _must("aura_source_to_ir_set_strict" in dirty, "AC1: C-linkage setter aura_source_to_ir_set_strict missing", fails)
    _must(
        "aura_source_to_ir_strict_v_read" in dirty,
        "AC1: C-linkage reader aura_source_to_ir_strict_v_read missing",
        fails,
    )
    _must("source_to_ir_hard_fail_total" in dirty, "AC1: hard_fail_total bump site missing in service_dirty.cpp", fails)
    _must("mark_all_blocks_dirty" in dirty, "AC1: full-relower force (mark_all_blocks_dirty) missing", fails)
    _must("finish_cascade_soa_dirty_sync_" in dirty, "AC1: cascade sync hook missing", fails)

    # AC2 — default Off (unit-test safe)
    _must("g_source_to_ir_strict{0}" in dirty, "AC2: g_source_to_ir_strict default must be 0 (Off)", fails)

    # AC1/AC4 — 2 atomic counters in observability_metrics.h
    _must(
        "source_to_ir_inconsistency_total{0}" in met,
        "AC4: source_to_ir_inconsistency_total counter missing in observability_metrics.h",
        fails,
    )
    _must(
        "source_to_ir_hard_fail_total{0}" in met,
        "AC4: source_to_ir_hard_fail_total counter missing in observability_metrics.h",
        fails,
    )

    # AC4 — 2 new query keys + schema-2244 lineage on query:incremental-relower-stats
    _must(
        "source-to-ir-inconsistency-total" in q,
        "AC4: source-to-ir-inconsistency-total key missing in evaluator_primitives_obs_eval.cpp",
        fails,
    )
    _must(
        "source_to_ir_inconsistency_total" in q,
        "AC4: source_to_ir_inconsistency_total (underscore form) key missing",
        fails,
    )
    _must(
        "source-to-ir-hard-fail-total" in q,
        "AC4: source-to-ir-hard-fail-total key missing in evaluator_primitives_obs_eval.cpp",
        fails,
    )
    _must("source_to_ir_hard_fail_total" in q, "AC4: source_to_ir_hard_fail_total (underscore form) key missing", fails)
    _must("schema-2244" in q and "issue-2244" in q, "AC4: schema-2244 / issue-2244 lineage missing", fails)

    # AC5 — test surface covers 3 new ACs (AC7/8/9 in #2045 test file)
    _must(
        "ac7_ensure_helper_consistent" in test,
        "AC5: AC7 (consistent path) missing in test_source_to_ir_map_consistency_2045.cpp",
        fails,
    )
    _must(
        "ac8_ensure_helper_strict_hard_fail" in test,
        "AC5: AC8 (Strict desync) missing in test_source_to_ir_map_consistency_2045.cpp",
        fails,
    )
    _must(
        "ac9_query_surface" in test,
        "AC5: AC9 (query surface) missing in test_source_to_ir_map_consistency_2045.cpp",
        fails,
    )
    _must("#2244" in test, "AC5: #2244 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2244 source_to_ir Strict coverage linter")
    parser.add_argument(
        "--self-test", action="store_true", help="Run self-test (return 0 if this linter parses + finds itself)"
    )
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

    print("OK: source_to_ir Strict coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

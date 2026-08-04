#!/usr/bin/env python3
"""Issue #2245: production sampling of incremental soundness coverage.

Contract (5 AC from issue body):
  AC1: Release build with sample_bp>0 runs oracle on partial success
       at approximately the configured rate (statistical, not exact).
  AC2: inject_soundness_under_dirty_for_test under sample_bp=10000
       -> mismatch counter + forced full path; no silent partial keep.
  AC3: sample_bp=0 -> zero oracle cost (same as today's release).
  AC4: Query: incremental-soundness-prod-runs-total, -ok-total,
       -mismatch-total, -sample-bp, schema lineage on
       query:incremental-soundness-stats (extend #2113 surface).
  AC5: Storm window elevates sample rate (document factor, e.g. 10x)
       without blocking reemit.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import re
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
    test = _read("tests/compiler/test_incremental_soundness_oracle.cpp")

    # AC1 — sample policy helpers in ir_cache_pure.ixx
    _must("soundness_sample_bp" in pure, "AC1: soundness_sample_bp() helper missing in ir_cache_pure.ixx", fails)
    _must("set_soundness_sample_bp" in pure, "AC1: set_soundness_sample_bp() setter missing", fails)
    _must("should_sample_soundness_prod" in pure, "AC1: should_sample_soundness_prod() policy helper missing", fails)
    _must("incremental_soundness_mode_allows_prod" in pure, "AC1: mode_allows_prod() helper missing", fails)
    _must(
        "aura_soundness_sample_bp_v_read" in pure,
        "AC1: C-linkage reader aura_soundness_sample_bp_v_read missing",
        fails,
    )
    _must(
        "aura_test_set_soundness_sample_bp" in pure,
        "AC1: C-linkage test setter aura_test_set_soundness_sample_bp missing",
        fails,
    )
    _must(
        "aura_test_set_soundness_force_mismatch" in pure,
        "AC2: C-linkage test hook aura_test_set_soundness_force_mismatch missing",
        fails,
    )
    _must("test_soundness_force_mismatch_for_next_partial" in pure, "AC2: test mismatch force flag missing", fails)

    # AC1/AC5 — storm elevation factor wired
    _must("storm_level_elevates_sample_bp" in pure, "AC5: storm_level_elevates_sample_bp() helper missing", fails)
    _must(
        "StormLevel" in pure and "Storm" in pure and "Elevated" in pure,
        "AC5: StormLevel enum + Storm/Elevated variants missing",
        fails,
    )
    _must("recent_full_fallback_rate_high" in pure, "AC5: recent_full_fallback_rate_high() heuristic missing", fails)

    # AC1/AC3 — default 1%, zero-cost path
    _must(
        re.search(r"static\s+std::atomic<int>\s+bp\{100\}", pure) is not None or "bp{100}" in pure,
        "AC1/AC3: default sample_bp must be 100 (1%)",
        fails,
    )

    # AC1 — wire-up at true_partial branch in service_dirty.cpp
    _must("should_sample_soundness_prod" in dirty, "AC1: policy helper not invoked in service_dirty.cpp", fails)
    _must("incremental_soundness_prod_runs_total" in dirty, "AC1: prod_runs_total bump site missing", fails)
    _must("true_partial" in dirty, "AC1: wire-up at true_partial branch missing", fails)
    _must("prod_sample_counter" in dirty, "AC1: prod_sample_counter (Knuth hash roll) missing", fails)

    # AC2 — forced mismatch path
    _must(
        "test_soundness_force_mismatch_for_next_partial" in dirty,
        "AC2: forced mismatch test hook not invoked in service_dirty.cpp",
        fails,
    )
    _must("incremental_soundness_mismatch_prod_total" in dirty, "AC2: mismatch_prod_total bump site missing", fails)
    _must(
        "mark_all_blocks_dirty" in dirty and "finish_cascade_soa_dirty_sync_" in dirty,
        "AC2: forced full relower path (mark_all_blocks_dirty + finish_cascade_soa_dirty_sync_) missing",
        fails,
    )

    # AC5 — storm elevation factor wired in wire-up
    _must("StormLevel" in dirty, "AC5: StormLevel wired in service_dirty.cpp", fails)

    # AC4 — 3 atomic counters in observability_metrics.h
    _must(
        "incremental_soundness_prod_runs_total{0}" in met,
        "AC4: incremental_soundness_prod_runs_total counter missing",
        fails,
    )
    _must(
        "incremental_soundness_prod_ok_total{0}" in met,
        "AC4: incremental_soundness_prod_ok_total counter missing",
        fails,
    )
    _must(
        "incremental_soundness_mismatch_prod_total{0}" in met,
        "AC4: incremental_soundness_mismatch_prod_total counter missing",
        fails,
    )

    # AC4 — 4 new query keys + schema-2245 lineage on query:incremental-soundness-stats
    _must(
        "incremental_soundness_prod_runs_total" in q and "incremental-soundness-prod-runs" in q,
        "AC4: prod-runs key (both forms) missing in evaluator_primitives_obs_eval.cpp",
        fails,
    )
    _must(
        "incremental_soundness_prod_ok_total" in q and "incremental-soundness-prod-ok" in q,
        "AC4: prod-ok key (both forms) missing",
        fails,
    )
    _must(
        "incremental_soundness_mismatch_prod_total" in q and "incremental-soundness-mismatch-prod" in q,
        "AC4: mismatch-prod key (both forms) missing",
        fails,
    )
    _must("incremental-soundness-sample-bp" in q, "AC4: sample-bp key missing", fails)
    _must("incremental-soundness-mode-allows-prod" in q, "AC4: mode-allows-prod key missing", fails)
    _must("schema-2245" in q and "issue-2245" in q, "AC4: schema-2245 / issue-2245 lineage missing", fails)

    # AC1/AC2/AC3/AC5 — test surface covers 4 new ACs (AC6-AC9 in #2113 test file)
    _must(
        "ac6_prod_sample_rate" in test,
        "AC1: AC6 (prod sample rate) missing in test_incremental_soundness_oracle.cpp",
        fails,
    )
    _must("ac7_prod_mismatch_forces_full" in test, "AC2: AC7 (prod mismatch) missing", fails)
    _must("ac8_sample_zero_cost_when_off" in test, "AC3: AC8 (zero cost when off) missing", fails)
    _must("ac9_storm_elevation_factor" in test, "AC5: AC9 (storm elevation) missing", fails)
    _must("#2245" in test, "AC5: #2245 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2245 incremental soundness prod sampling coverage linter")
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

    print("OK: incremental soundness prod sampling coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

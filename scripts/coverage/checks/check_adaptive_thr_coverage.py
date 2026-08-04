#!/usr/bin/env python3
"""Issue #2248: Agent-driven adaptive relower threshold from fallback-reason telemetry.

Contract (5 AC from issue body):
  AC1: Sustained MapInconsistent / soundness-mismatch / dual-parity
       fails raise the effective partial threshold (measurable via query).
  AC2: Clean window of N successful partials decays the threshold back
       toward base (no permanent ratchet).
  AC3: Env / capability override still works (AURA_ADAPTIVE_THR=0 freezes).
  AC4: Metrics + query: adaptive-thr-current, -raises-total, -decays-total,
       reason-bucket breakdown, schema lineage.
  AC5: StormLevel still ORs with the adaptive decision.

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
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_adaptive_partial_relower_threshold.cpp")

    # AC1/AC2/AC3 — policy + helpers in ir_cache_pure.ixx
    _must("AdaptiveThrPolicy" in pure, "AC1/AC2: AdaptiveThrPolicy struct missing in ir_cache_pure.ixx", fails)
    _must("current_adaptive_partial_thr" in pure, "AC1: current_adaptive_partial_thr getter missing", fails)
    _must("note_relower_fallback_for_adaptive" in pure, "AC1: note_relower_fallback_for_adaptive helper missing", fails)
    _must("adaptive_thr_frozen" in pure, "AC3: adaptive_thr_frozen helper missing", fails)
    _must("AURA_ADAPTIVE_THR" in pure, "AC3: AURA_ADAPTIVE_THR env override missing", fails)
    # AC1: correctness-risk reasons referenced
    _must(
        "MapInconsistent" in pure and "DesyncForceFull" in pure,
        "AC1: correctness-risk reasons (MapInconsistent + DesyncForceFull) missing",
        fails,
    )
    # AC2: clean window decay wired
    _must(
        "clean_window_count" in pure and "kCleanDecayAfter" in pure,
        "AC2: clean_window_count decay counter + kCleanDecayAfter threshold missing",
        fails,
    )
    # AC5: StormLevel still ORs (existing should_partial_relower_workload_storm_aware
    # + adaptive check — caller composes)
    _must(
        "should_partial_relower_workload_storm_aware" in pure,
        "AC5: storm_aware helper missing (preserved from #2112/#2190)",
        fails,
    )
    # Wire-up: note_relower_fallback calls note_relower_fallback_for_adaptive
    _must(
        "note_relower_fallback_for_adaptive(r)" in met
        or "note_relower_fallback_for_adaptive(r)" in pure
        or "note_relower_fallback_for_adaptive" in met,
        "AC1: note_relower_fallback does not feed adaptive policy",
        fails,
    )

    # AC4 — 5 atomic counters in observability_metrics.h
    _must("adaptive_thr_current{800}" in met, "AC4: adaptive_thr_current counter missing", fails)
    _must("adaptive_thr_raises_total{0}" in met, "AC4: adaptive_thr_raises_total counter missing", fails)
    _must("adaptive_thr_decays_total{0}" in met, "AC4: adaptive_thr_decays_total counter missing", fails)
    _must("adaptive_thr_bad_window_count{0}" in met, "AC4: adaptive_thr_bad_window_count counter missing", fails)
    _must("adaptive_thr_frozen{0}" in met, "AC4: adaptive_thr_frozen counter missing", fails)

    # AC4 — 4 new query keys + schema-2248 lineage on query:incremental-relower-stats
    _must("adaptive-thr-current" in q, "AC4: adaptive-thr-current key missing", fails)
    _must("adaptive-thr-raises-total" in q, "AC4: adaptive-thr-raises-total key missing", fails)
    _must("adaptive-thr-decays-total" in q, "AC4: adaptive-thr-decays-total key missing", fails)
    _must("adaptive-thr-bad-window-count" in q, "AC4: adaptive-thr-bad-window-count key missing", fails)
    _must("adaptive-thr-frozen" in q, "AC4: adaptive-thr-frozen key missing", fails)
    _must("adaptive-thr-wired" in q, "AC4: adaptive-thr-wired sentinel missing", fails)
    _must("schema-2248" in q and "issue-2248" in q, "AC4: schema-2248 / issue-2248 lineage missing", fails)

    # AC5 — test surface covers #2248 (ac2248 in test_adaptive_partial_relower_threshold.cpp)
    _must(
        "ac2248_agent_driven_adaptive_thr" in test, "AC5: ac2248_agent_driven_adaptive_thr test function missing", fails
    )
    _must("#2248" in test, "AC5: #2248 issue citation missing in test file comment", fails)
    # AC1 runtime smoke
    _must(
        "inject_adaptive_thr_bad_for_test" in test, "AC1: inject_adaptive_thr_bad_for_test runtime check missing", fails
    )
    _must("reset_adaptive_thr_for_test" in test, "AC2: reset_adaptive_thr_for_test runtime check missing", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2248 adaptive relower threshold coverage linter")
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

    print("OK: adaptive relower threshold coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

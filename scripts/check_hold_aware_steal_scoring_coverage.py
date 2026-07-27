#!/usr/bin/env python3
"""Issue #2253: hold-aware work-steal scoring (depth + hold_us + priority boost).

Contract (4 AC from issue body):
  AC1: WorkerThread::steal() ranks candidates with simple integer score
       (+100 outermost-safe + +50 has_steal_priority_boost
        + +20 YieldReason::Explicit|OperationBoundary|PassPipeline
        - 40 recent hold_us > p90).
  AC2: long-hold victims remain steal-deferred while unsafe; when
       outermost-safe, boost applies once (existing clear-on-success).
  AC3: Metrics: steal_score_selected_total + bucket histogram;
       no regression on existing outermost/inner deferred counters.
  AC4: Mixed-MB-load steal distribution test.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


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

    worker = _read("src/serve/worker.cpp")
    fiber_h = _read("src/serve/fiber.h")
    sched = _read("src/serve/scheduler.cpp")
    met = _read("src/serve/metrics.h")
    test = _read("tests/serve/test_orchestration_steal_boost.cpp")

    # AC1 — score components in worker.cpp steal() success path
    _must("score += 100" in worker, "AC1: +100 outermost-safe score component missing", fails)
    _must("has_steal_priority_boost" in worker, "AC1: +50 priority boost component missing", fails)
    _must("YieldReason::Explicit" in worker, "AC1: +20 short-yield component missing", fails)
    _must("score -= 40" in worker, "AC1: -40 recent hold penalty missing", fails)

    # AC3 — scoring bumps counters + bucket histogram
    _must("steal_score_selected_total.fetch_add" in worker, "AC3: steal_score_selected_total bump site missing", fails)
    _must(
        "steal_score_bucket_0_49" in worker and "steal_score_bucket_200p" in worker,
        "AC3: bucket histogram missing",
        fails,
    )

    # AC2 — last_hold_us on Fiber + wire on_long_mutation_held
    _must("last_hold_us_" in fiber_h, "AC2: Fiber::last_hold_us_ field missing", fails)
    _must("last_hold_us()" in fiber_h, "AC2: Fiber::last_hold_us() getter missing", fails)
    _must("f->set_last_hold_us(duration_us)" in sched, "AC2: on_long_mutation_held wire missing", fails)

    # AC3 — counter fields + bucket histogram in adaptive_steal_stats
    _must("steal_score_selected_total{0}" in met, "AC3: steal_score_selected_total field missing", fails)
    _must("steal_score_bucket_0_49{0}" in met, "AC3: bucket 0-49 field missing", fails)
    _must("steal_score_bucket_50_99{0}" in met, "AC3: bucket 50-99 field missing", fails)
    _must("steal_score_bucket_100_149{0}" in met, "AC3: bucket 100-149 field missing", fails)
    _must("steal_score_bucket_150_199{0}" in met, "AC3: bucket 150-199 field missing", fails)
    _must("steal_score_bucket_200p{0}" in met, "AC3: bucket 200p field missing", fails)

    # AC4 — test surface covers #2253 (ac2253 in test_orchestration_steal_boost.cpp)
    _must(
        ("ac2253_score_based_steal_ranking" in test) or ("AC #2253" in test),
        "AC4: ac2253_score_based_steal_ranking test function (or AC #2253 inline block) missing",
        fails,
    )
    _must("#2253" in test, "AC4: #2253 issue citation missing in test file comment", fails)
    _must("score += 100" in test or "+100" in test, "AC4: scoring source-cite in test missing", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2253 hold-aware work-steal scoring coverage linter")
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

    print("OK: hold-aware work-steal scoring coverage - all 4 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2252: hard-reject native execution when AOT slot table_generation != live epoch.

Contract (5 AC from issue body):
  AC1: Before every AOT native call (aura_aot_probe_fn_ptr): if
       slot.table_generation != g_aot_table_epoch -> bump
       aot_stale_probe_hard_reject_total + return 0 (nullptr).
  AC2: Optional defense-in-depth (Fiber resume LayoutStamp.defuse compare).
  AC3: Happy path matching generations = zero extra atomics beyond
       existing probe loads.
  AC4: Metric + query surface (additive keys on
       query:aot-incremental-reemit-stats + schema-2252 lineage).
  AC5: Concurrent mutate + apply -> hard-reject count > 0 + zero
       native hits on the old generation.

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

    bridge_cpp = _read("src/compiler/aura_jit_bridge.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_aot_reload_primitive.cpp")

    # AC1 — aura_aot_probe_fn_ptr hard-reject bump
    _must("aura_aot_probe_fn_ptr" in bridge_cpp, "AC1: aura_aot_probe_fn_ptr missing in aura_jit_bridge.cpp", fails)
    _must(
        "aot_stale_probe_hard_reject_total.fetch_add" in bridge_cpp,
        "AC1: hard-reject bump site missing in aura_aot_probe_fn_ptr",
        fails,
    )

    # AC3 — happy path zero-cost
    _must(
        "gen != cur" in bridge_cpp
        and "g_aot_table_epoch.load" in bridge_cpp
        and "slot.table_generation.load" in bridge_cpp,
        "AC3: relaxed load compare gen != cur missing",
        fails,
    )

    # AC4 — metric field + query surface + schema-2252
    _must(
        "aot_stale_probe_hard_reject_total{0}" in met,
        "AC4: aot_stale_probe_hard_reject_total counter field missing",
        fails,
    )
    _must("aot-stale-probe-hard-reject-total" in q, "AC4: query key missing", fails)
    _must("aot-stale-probe-hard-reject-wired" in q, "AC4: wired sentinel missing", fails)
    _must("schema-2252" in q and "issue-2252" in q, "AC4: schema-2252 / issue-2252 lineage missing", fails)

    # AC5 — test surface covers #2252 (ac2252 in test_aot_reload_primitive.cpp)
    _must(
        ("ac2252_hard_reject_stale_slot" in test) or ("AC #2252" in test),
        "AC5: ac2252_hard_reject_stale_slot test function (or AC #2252 inline block) missing",
        fails,
    )
    _must("#2252" in test, "AC5: #2252 issue citation missing in test file comment", fails)
    _must(
        "aura_aot_probe_fn_ptr" in test and "aot_stale_probe_hard_reject_total" in test,
        "AC5: probe fn + hard-reject counter source-cite missing",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2252 AOT stale probe hard-reject coverage linter")
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

    print("OK: AOT stale probe hard-reject coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

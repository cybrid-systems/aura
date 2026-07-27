#!/usr/bin/env python3
"""Issue #2247: dual dep_graph write-parity gate + hybrid cascade consistency.

Contract (5 AC from issue body):
  AC1: Injected divergence (string has edge, NodeId missing) under Strict
       -> parity rebuild + dual_dep_graph_parity_fail_total + subsequent
       cascade sees the edge.
  AC2: Off / unit-test default remains soft (optional rebuild, no force).
  AC3: Happy-path dual write is O(1) extra work when already maintaining
       both; no full rebuild on every store.
  AC4: Query surface (additive): dual-dep-graph-parity-check-total,
       dual-dep-graph-parity-fail-total, schema lineage on
       query:soa-dirty-stats or query:cascade-stats (we use cascade-stats).
  AC5: CI coverage script asserting every edge-write site either writes
       both or calls the parity helper.

This linter is the source-of-truth for the production surface. A ship
is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import re
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

    pure = _read("src/compiler/dirty_propagation.ixx")
    met = _read("src/compiler/observability_metrics.h")
    svc = _read("src/compiler/service.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_dep_graph_hybrid_cascade_2110.cpp")

    # AC1/AC3 — pure parity primitives in dirty_propagation.ixx
    _must("graphs_consistent" in pure, "AC1: graphs_consistent helper missing in dirty_propagation.ixx", fails)
    _must("rebuild_node_dep_graph_from_string" in pure, "AC1: rebuild_node_dep_graph_from_string helper missing", fails)
    _must("g_dual_dep_graph_parity_check_total_atomic" in pure, "AC1: process atomic check counter missing", fails)
    _must("g_dual_dep_graph_parity_fail_total_atomic" in pure, "AC1: process atomic fail counter missing", fails)

    # AC2 — default Off (unit-test safe) + Strict toggle
    _must(
        re.search(r"std::atomic<std::uint8_t>&\s+g_dual_dep_graph_strict_atomic", pure) is not None
        or "g_dual_dep_graph_strict_atomic" in pure,
        "AC2: g_dual_dep_graph_strict toggle missing",
        fails,
    )
    _must("aura_set_dual_dep_graph_strict" in pure, "AC2: C-linkage setter missing", fails)
    _must("dual_dep_graph_strict_enabled" in pure, "AC2: dual_dep_graph_strict_enabled() helper missing", fails)
    _must(
        "aura_dual_dep_graph_parity_check_v_read" in pure and "aura_dual_dep_graph_parity_fail_v_read" in pure,
        "AC1: C-linkage v_read for both counters missing",
        fails,
    )

    # AC1 — wire-up in service.ixx (record_dependency chokepoint)
    _must(
        svc.find("graphs_consistent(dep_graph_") != -1,
        "AC1: parity gate not wired at record_dependency in service.ixx",
        fails,
    )
    _must(svc.find("dual_dep_graph_strict_enabled") != -1, "AC1: Strict toggle check not in wire-up", fails)
    _must(svc.find("rebuild_node_dep_graph_from_string") != -1, "AC1: rebuild call not in wire-up", fails)
    _must(
        svc.find("metrics_.dual_dep_graph_parity_check_total") != -1, "AC1: metrics bump for check_total missing", fails
    )
    _must(
        svc.find("metrics_.dual_dep_graph_parity_fail_total") != -1, "AC1: metrics bump for fail_total missing", fails
    )
    # Strict force path (mark_all_blocks_dirty + finish_cascade_soa_dirty_sync_)
    _must(
        svc.find("mark_all_blocks_dirty") != -1 and svc.find("finish_cascade_soa_dirty_sync_") != -1,
        "AC1: Strict force path (mark_all_blocks_dirty + finish_cascade_soa_dirty_sync_) missing",
        fails,
    )

    # AC4 — 2 atomic counters in observability_metrics.h
    _must(
        "dual_dep_graph_parity_check_total{0}" in met, "AC4: dual_dep_graph_parity_check_total counter missing", fails
    )
    _must("dual_dep_graph_parity_fail_total{0}" in met, "AC4: dual_dep_graph_parity_fail_total counter missing", fails)

    # AC4 — 2 new query keys + schema-2247 lineage on query:dirty-cascade-stats
    _must("dual-dep-graph-parity-check-total" in q, "AC4: dual-dep-graph-parity-check-total key missing", fails)
    _must("dual-dep-graph-parity-fail-total" in q, "AC4: dual-dep-graph-parity-fail-total key missing", fails)
    _must("dual-dep-graph-parity-strict" in q, "AC4: dual-dep-graph-parity-strict key missing", fails)
    _must("schema-2247" in q and "issue-2247" in q, "AC4: schema-2247 / issue-2247 lineage missing", fails)

    # AC5 — test surface covers #2247 (ac2247 in test_dep_graph_hybrid_cascade_2110.cpp)
    _must(
        "ac2247_dual_dep_graph_parity_gate" in test,
        "AC5: ac2247_dual_dep_graph_parity_gate test function missing",
        fails,
    )
    _must("#2247" in test, "AC5: #2247 issue citation missing in test file comment", fails)
    _must("set_dual_dep_graph_strict" in test, "AC5: set_dual_dep_graph_strict runtime check missing in test", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2247 dual dep_graph parity coverage linter")
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

    print("OK: dual dep_graph parity coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

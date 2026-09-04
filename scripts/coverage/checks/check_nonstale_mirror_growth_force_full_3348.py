#!/usr/bin/env python3
"""Issue #3348: non-stale NodeId / block-dep mirror last-look before partial.

#3283 closed stale-reject → deferred_hybrid_gen_ re-arm before partial peel.
Residual: the non-stale success path of record_dependency /
record_block_dependency still takes only dep_graph_mtx_ (not
cascade_decision_mtx_). While relower holds the cascade lock and peels
partial, a concurrent fiber can mirror new NodeId / block-dep edges
(dep_graph_node_mirror_edges_total / dep_graph_block_mirror_edges_total
advance). Mid-loop graph_grew_mid_loop / #3168 / #3257 / #3283 were
built around the deferred queue and do not last-look these live mirrors
immediately before peel → silent under-cascade.

Fix (fail-closed, no new pipeline / query key):
- Snapshot node + block mirror counters with gen0 / deferred size.
- graph_grew_mid_loop observes both counters.
- Last-look immediately before committing partial: counter advanced →
  mark_all_blocks_dirty + force full (partial_forced_full_by_impact_total).
- Soft + counters flat: one acquire each, zero extra.

Contract:
  AC1 last-look + both snapshots; reuse force-full distinguisher
  AC2 graph_grew_mid_loop includes block-dep; #3283 gen re-check retained
  AC3 concurrent non-stale soak in test_cascade_decision_residual_atomic
  AC4 after #3283; no invent / docs/design / g_3348_* / schema-3348

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_cascade_decision_residual_atomic.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp") + _read("src/compiler/evaluator_primitives_query.cpp")

    rel_pos = svc.find("std::size_t relower_dirty_defines_from_workspace()")
    # Window expanded after #3484 zero-mask fail-closed grew the peel
    # body (last-look node/block compares sat past 28000).
    rel = svc[rel_pos : rel_pos + 32000] if rel_pos >= 0 else ""

    must("Issue #3348", "AC1 cite", rel)
    must("initial_block_mirror_edges", "AC1 block snapshot", rel)
    must("initial_node_mirror_edges", "AC1 node snapshot", rel)
    must("node_now > initial_node_mirror_edges", "AC1 last-look node", rel)
    must("block_now > initial_block_mirror_edges", "AC1 last-look block", rel)
    must("partial_forced_full_by_impact_total", "AC1 distinguisher", rel)
    must("ac3348_1_last_look_source", "AC1 test", t)
    if "schema-3348" in q:
        fails.append("AC1: new schema-3348 query key")
    if "g_3348_" in svc:
        fails.append("AC1: new g_3348_* counter")

    must("dep_graph_block_mirror_edges_total", "AC2 block observe", rel)
    must("graph_grew_mid_loop", "AC2 mid-loop retained", rel)
    must("deferred_hybrid_gen_.load(std::memory_order_acquire) != gen0", "AC2 #3283 retained", rel)
    must("ac3348_2_soft_quiet", "AC2 test", t)

    must("ac3348_3_concurrent_nonstale_soak", "AC3 soak", t)
    must("public_record_block_dependency", "AC3 block inject", t)
    must("public_record_dependency", "AC3 node inject", t)

    must("check_nonstale_mirror_growth_force_full_3348", "AC4 build.py", build)
    must("check_deferred_rearm_lag_3283", "AC4 after #3283", build)
    i3283 = build.find("check_deferred_rearm_lag_3283")
    i3348 = build.find("check_nonstale_mirror_growth_force_full_3348")
    if i3283 < 0 or i3348 < 0 or i3348 < i3283:
        fails.append("AC4: #3348 linter must run after #3283")
    must("ac3348_4_linter_no_invent", "AC4 test", t)
    if (ROOT / "tests" / "issues" / "test_issue_3348.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3348.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3348.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3348.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3348-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3348 nonstale_mirror_growth_force_full:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3348 nonstale_mirror_growth_force_full: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

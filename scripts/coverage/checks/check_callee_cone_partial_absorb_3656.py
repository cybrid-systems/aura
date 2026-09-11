#!/usr/bin/env python3
"""Issue #3656: caller partial must absorb callee cone (block units).

#3584 stopped mixing callee *define count* into estimate_relower_blocks.
precompute still early-returned 0 when string `calls` was empty, so a
nonempty source_to_ir_map + empty calls looked like "no callee" even
when node_dep still had encode_fn_node edges. Caller dirty_n==1 then
should_partial_relower with a clean Call block.

Contract (one row per AC):
  AC1  Production: empty calls + node fn edges → kUnknownCalleeConeBlocks;
       peel fail-closed full (partial_forced_full_by_impact_total)
  AC2  #3584: peel uses 2-arg estimate_relower_blocks; clean callee cone
       does not bump impact_ub (no define-count mix)
  AC3  map empty still returns 0; absorb leaves ub==0 / -1 ( #3310 )
  AC4  Soft precompute still observe-only return 0
  AC5  extends test_partial_relower_cascade +
       test_dep_graph_partial_relower_threshold; linter AFTER #3653;
       no test_issue_3656.cpp; no docs/design/; no
       query:incremental-relower-stats rewrite

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    dirty = _read("src/compiler/service_dirty.cpp")
    svc = _read("src/compiler/service.ixx")
    pure = _read("src/compiler/ir_cache_pure.ixx")
    prop = _read("src/compiler/dirty_propagation.ixx")
    cas = _read("tests/compiler/test_partial_relower_cascade.cpp")
    thr = _read("tests/compiler/test_dep_graph_partial_relower_threshold.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    pre_at = dirty.find("CompilerService::precompute_callee_cascade_for_partial")
    pre = dirty[pre_at : pre_at + 4200] if pre_at >= 0 else ""

    must("Issue #3656", "AC1 precompute cite", dirty)
    must("kUnknownCalleeConeBlocks", "AC1 unknown sentinel", pre)
    must("node_dep_has_fn_edges_for_slot", "AC1 node consult", pre)
    must("calls.empty()", "AC1 empty-calls arm", pre)
    must("kUnknownCalleeConeBlocks", "AC1 peel unknown", svc)
    must("partial_forced_full_by_impact_total", "AC1 reuse counter", svc)
    must("ac3656_1_empty_calls_node_fn_forces_full", "AC1 cascade test", cas)
    must("node_dep_has_fn_edges_for_slot", "AC1 helper", prop)

    must("estimate_relower_blocks(dirty_n, get_partial_relower_threshold())", "AC2 2-arg estimate", svc)
    must_not(
        "estimate_relower_blocks(dirty_n, get_partial_relower_threshold(),",
        "AC2 no 3-arg define mix at peel",
        svc,
    )
    must("already_dirty_blocks", "AC2 already-dirty only", pre)
    must("absorb_callee_cone_into_impact_ub", "AC2 absorb into ub", svc)
    must("ac3656_2_hub_not_define_count_full", "AC2 cascade test", cas)
    must("ac3584_1_hub_partial_peel", "AC2 3584 retained", cas)

    must("source_to_ir_map.empty()", "AC3 map-empty return 0", pre)
    must("absorb_callee_cone_into_impact_ub", "AC3 absorb helper", pure)
    must("ac3656_3_map_empty_unknown_full", "AC3 cascade test", cas)
    must("should_partial_relower_impact_checked_prod", "AC3 3310 helper", pure)

    must("g_partial_relower_callee_cascade_precompute_observe_total", "AC4 Soft observe", pre)
    must("return 0;", "AC4 Soft return 0", pre)
    must("ac3656_4_soft_zero_extra", "AC4 cascade test", cas)

    must("check_outermost_persist_audit_order_3653", "AC5 prev linter", build)
    must("check_callee_cone_partial_absorb_3656", "AC5 build.py", build)
    prev = build.find("check_outermost_persist_audit_order_3653")
    ours = build.find("check_callee_cone_partial_absorb_3656")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3653")
    must("ac3656_threshold_source_cite", "AC5 threshold suite", thr)
    must("query:incremental-relower-stats", "AC5 query retained", q)
    must_not("schema-3656", "AC5 no new query key", q)
    if _read("tests/compiler/test_issue_3656.cpp"):
        fails.append("AC5: test_issue_3656.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3656-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3656 callee_cone_partial_absorb:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3656 callee_cone_partial_absorb")
    return 0


if __name__ == "__main__":
    sys.exit(main())

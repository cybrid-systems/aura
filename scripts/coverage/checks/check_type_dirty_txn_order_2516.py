#!/usr/bin/env python3
"""Issue #2516: type_dep invalidate → partial re-infer → cascade mirror txn.

Contract:
  AC1 single ordered sequence on production partial typed_mutate paths
  AC2 stale type_dep edges dropped before re-infer
  AC3 cascade mirror after re-infer (post-infer cone)
  AC4 empty dirty → no invalidate / no mirror cost
  AC5 counters + query + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tci = _read("src/compiler/type_checker_impl.cpp")
    tch = _read("src/compiler/type_checker.ixx")
    dirty = _read("src/compiler/dirty_propagation.ixx")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    svc = _read("src/compiler/service.ixx")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    ev = _read("src/compiler/evaluator_primitives_eval.cpp")
    test = _read("tests/compiler/test_type_dirty_txn_order_2516.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2516", "AC1", tci)
    must("infer_flat_partial_with_dirty_txn", "AC1", tch)
    must("infer_flat_partial_with_dirty_txn", "AC1", svc)
    must("infer_flat_partial_with_dirty_txn", "AC1", etc)
    must("infer_flat_partial_with_dirty_txn", "AC1", ev)
    must("type_dirty_txn_phase1_invalidate_total", "AC1", tci)
    must("type_dirty_txn_phase2_reinfer_total", "AC1", tci)
    must("type_dirty_txn_phase3_mirror_total", "AC1", tci)
    must("ac1_single_ordered_sequence", "AC1", test)

    # AC2 — source order phase1 < phase2 < phase3
    p1 = tci.find("type_dirty_txn_phase1_invalidate_total")
    p2 = tci.find("type_dirty_txn_phase2_reinfer_total")
    p3 = tci.find("type_dirty_txn_phase3_mirror_total")
    if not (p1 >= 0 and p2 > p1 and p3 > p2):
        fails.append("AC2: phase counters not in order phase1 < phase2 < phase3")
    must("invalidate_type_dep_for_nodes", "AC2", tci)
    must("ac2_invalidate_before_reinfer", "AC2", test)

    # AC3
    must("mirror_type_affected_to_cascade", "AC3", tci)
    must("#2516", "AC3", dirty)
    mir_last = tci.rfind("mirror_type_affected_to_cascade")
    if mir_last < 0 or mir_last < p2:
        fails.append("AC3: last mirror call must be after phase2 reinfer marker")
    must("ac3_mirror_after_reinfer", "AC3", test)

    # AC4
    must("if (affected.empty())", "AC4", tci)
    must("affected_ast.empty()", "AC4", dirty)
    must("ac4_empty_dirty_zero_cost", "AC4", test)

    # AC5
    must("type_dirty_txn_order_wired", "AC5", met)
    must("type_dirty_txn_total", "AC5", met)
    must("type_dirty_txn_order_wired", "AC5", fields)
    must("schema-2516", "AC5", q)
    must("type-dirty-txn-order-wired", "AC5", q)
    must("type-dirty-txn-phase1-invalidate-total", "AC5", q)
    must("type-dirty-txn-phase3-mirror-total", "AC5", q)
    must("schema-2355", "AC5", q)
    must("type_dirty_cone_mirrored_total", "AC5", met)
    must("test_type_dirty_txn_order_2516", "AC5", cmake)
    must("check_type_dirty_txn_order_2516", "AC5", build)
    must("cmd_type_dirty_txn_order_coverage", "AC5", build)
    must("ac5_counters_and_query", "AC5", test)

    # Retain prior pieces
    must("type_dep_partial_merge_total", "retain", tci)
    must("prune_type_dep_graph", "retain", tci)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2516 type_dirty txn order — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

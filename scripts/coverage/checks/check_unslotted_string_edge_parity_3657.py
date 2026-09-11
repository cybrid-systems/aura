#!/usr/bin/env python3
"""Issue #3657: unslotted string edges are production parity misses.

graphs_consistent used to `continue` when a string called_by edge had no
dep_name_to_slot_ entry, so lockless reject / never-mirrored edges were
invisible to dual-graph parity. deferred_hybrid_pending_upper_bound_
returns 0 when armed==0 — reject must store(1).

Contract (one row per AC):
  AC1  Production/Full + string called_by without slot → graphs_consistent
       false; peel bumps dual_dep_graph_parity_fail_total or
       partial_forced_full_by_impact_total
  AC2  lockless reject stores deferred_hybrid_armed_=1; pending UB
       includes the edge
  AC3  Soft keeps unslotted continue; armed==0 zero cost
  AC4  slotted #3165 public_graphs_consistent soak stays
  AC5  extends test_cascade_decision_residual_atomic +
       test_dep_graph_hybrid_cascade; linter AFTER #3655; no
       test_issue_3657.cpp; no docs/design/; no query:dirty-cascade-stats
       rewrite

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

    pure = _read("src/compiler/dirty_propagation.ixx")
    svc = _read("src/compiler/service.ixx")
    hy = _read("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
    cas = _read("tests/compiler/test_cascade_decision_residual_atomic.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    win_at = pure.find("Issue #3657")
    pure[win_at : win_at + 1800] if win_at >= 0 else ""
    gc_at = pure.find("[[nodiscard]] inline bool graphs_consistent(")
    gc = pure[gc_at : gc_at + 1600] if gc_at >= 0 else ""

    must("Issue #3657", "AC1 graphs_consistent cite", pure)
    must("hard_unslotted", "AC1 production unslotted gate", gc)
    must("production_defaults_active()", "AC1 production face", gc)
    must("return false", "AC1 unslotted is miss", gc)
    must("inject_string_called_by_unslotted_for_test", "AC1 inject hook", svc)
    must("ac3657_1_unslotted_production_inconsistent", "AC1 hybrid test", hy)

    rd_at = svc.find("void record_dependency(const std::string& caller, const std::string& callee)")
    rd = svc[rd_at : rd_at + 4500] if rd_at >= 0 else ""
    must("Issue #3657", "AC2 reject cite", rd)
    must("deferred_hybrid_armed_.store(1", "AC2 reject arms", rd)
    must("public_deferred_hybrid_armed_for_test", "AC2 armed hook", svc)
    must("ac3657_2_reject_arms_pending_ub", "AC2 cascade test", cas)

    must("never mirrored; not a parity violation (Soft)", "AC3 Soft continue", gc)
    must("ac3657_3_soft_unslotted_continue", "AC3 hybrid test", hy)

    must("ac3165_strict_fail_closed_all_callers", "AC4 #3165 stays", hy)
    must("ac3657_4_slotted_3165_still_consistent", "AC4 slotted soak", hy)

    must("check_unslotted_string_edge_parity_3657", "AC5 build.py", build)
    prev = build.find("check_persist_sdo_before_unstaged_3655")
    ours = build.find("check_unslotted_string_edge_parity_3657")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3655")
    if "schema-3657" in q:
        fails.append("AC5: rewrote/added schema-3657 query key")
    if _read("tests/compiler/test_issue_3657.cpp"):
        fails.append("AC5: test_issue_3657.cpp present")
    if _read("docs/design/3657-unslotted-parity.md"):
        fails.append("AC5: docs/design/ exists")

    if fails:
        print("FAIL #3657 unslotted_string_edge_parity:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3657 unslotted_string_edge_parity")
    return 0


if __name__ == "__main__":
    sys.exit(main())

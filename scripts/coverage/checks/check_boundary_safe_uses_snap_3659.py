#!/usr/bin/env python3
"""Issue #3659: BoundarySafe uses the transaction snap, not a resample.

steal_safety_transaction samples snap then evaluate_residual_hard_and_bits
stamped snap.ticket on Ok. BoundarySafe called the no-argument
is_at_mutation_boundary_safe probe (third sample). snap.held==true with a
later held_mirror==0 could pass and enqueue a held-era ticket.

Contract (one row per AC):
  AC1  evaluate_residual_hard_and_bits calls is_at_mutation_boundary_safe(snap);
       steal_safety.cpp has no is_at_mutation_boundary_safe()
  AC2  held snap + later held_mirror==0 → BoundarySafe fail; no ticket stamp
  AC3  clean snap still Ok; ticket==snap.ticket; GcDefer/EnvFrame/LCP
       still victim eval id
  AC4  Soft/single-worker not forced; Hard still RejectHard on held snap
  AC5  extends test_is_stealable_snapshot_gate; #3621 TLS enum stays;
       linter AFTER #3658; no test_issue_3659.cpp; no docs/design/;
       no query-key rewrite

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

    ss = _read("src/serve/steal_safety.cpp")
    t = _read("tests/serve/test_is_stealable_snapshot_gate.cpp")
    inv = _read("tests/serve/test_steal_snapshot_hard_invariant.cpp")
    build = _read("build.py")

    win_at = ss.find("evaluate_residual_hard_and_bits(Fiber* stolen")
    win = ss[win_at : win_at + 2500] if win_at >= 0 else ""

    must("Issue #3659", "AC1 cite", ss)
    must("is_at_mutation_boundary_safe(snap)", "AC1 snap overload", win)
    if "is_at_mutation_boundary_safe()" in ss:
        fails.append("AC1: steal_safety.cpp still has parameterless is_at_mutation_boundary_safe()")
    must("ac3659_1_boundary_safe_uses_snap", "AC1 test", t)

    must("ac3659_2_held_snap_rejects_after_clear", "AC2 test", t)
    must("set_resume_safety_ticket(snap.ticket)", "AC2 stamp still Ok-only", ss)

    must("ac3659_3_clean_snap_ok_victim_id", "AC3 test", t)
    must("aura_fiber_evaluator_id_for_steal_safety(stolen)", "AC3 victim id", ss)

    must("ac3659_4_soft_unchanged", "AC4 test", t)
    must("Soft: skip entirely (no loads)", "AC4 Lifetime Soft skip", ss)

    must("check_boundary_safe_uses_snap_3659", "AC5 build.py", build)
    prev = build.find("check_type_dirty_txn_before_cascade_3658")
    ours = build.find("check_boundary_safe_uses_snap_3659")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3658")
    must("ac3621_1_depth_victim_storage_identity", "AC5 #3621 stays", inv)
    if _read("tests/serve/test_issue_3659.cpp"):
        fails.append("AC5: test_issue_3659.cpp present")
    if _read("docs/design/3659-boundary-safe-snap.md"):
        fails.append("AC5: docs/design/ exists")
    if "schema-3659" in ss or "g_3659_" in ss:
        fails.append("AC5: new query key / counter")

    if fails:
        print("FAIL #3659 boundary_safe_uses_snap:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3659 boundary_safe_uses_snap")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2929: steal_safety_transaction hard-AND as StealInvariant table.

Residual of #2844/#2721/#2699: residual arms are correct but not named /
not individually first-class for Agents / linters. Make sole-enqueue gate
provable by construction.

Contract (one row per AC):
  AC1 every residual arm maps to StealInvariant + dedicated fail counter
  AC2 ticket stamped only after all invariants pass (#2844 preserved)
  AC3 Soft metric-only; production fail-closed (no Soft continue enqueue)
  AC4 query schema-2929 + per-invariant totals; #2699/#2721 non-regressing
  AC5 extend test_steal_complete_restamp_txn; linter wired; no invent
  AC6 no docs/design/*

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

    hdr = _read("src/serve/steal_safety.h")
    ss = _read("src/serve/steal_safety.cpp")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1 — named table + counters
    must("enum class StealInvariant", "AC1", hdr)
    must("SnapshotConsistent", "AC1", hdr)
    must("BoundarySafe", "AC1", hdr)
    must("LayoutStampMatch", "AC1", hdr)
    must("TicketFresh", "AC1", hdr)
    must("GcDeferClear", "AC1", hdr)
    must("EnvFrameOk", "AC1", hdr)
    must("steal_invariant_mask", "AC1", hdr)
    must("g_steal_safety_invariant_snapshot_fail_total", "AC1", hdr)
    must("g_steal_safety_last_reject_invariant_bits", "AC1", hdr)
    must("steal_safety_invariant_fail_total", "AC1", hdr)
    must("note_steal_invariant_fail", "AC1", ss)
    must("StealInvariant::BoundarySafe", "AC1 body", ss)
    must("StealInvariant::LayoutStampMatch", "AC1 body", ss)
    must("StealInvariant::TicketFresh", "AC1 body", ss)
    must("StealInvariant::GcDeferClear", "AC1 body", ss)
    must("StealInvariant::EnvFrameOk", "AC1 body", ss)
    must("evaluate_residual_hard_and_bits", "AC1", ss)
    must("kStealSafetyInvariantTableIssue", "AC1", hdr)

    # AC2 — ticket only on Ok after all pass
    must("stolen->set_resume_safety_ticket(snap.ticket)", "AC2", ss)
    must("if (!residual_ok)", "AC2", ss)
    must("all invariants Ok", "AC2", ss)
    # Ticket stamp not before residual hard-AND decision
    stamp = ss.find("stolen->set_resume_safety_ticket(snap.ticket)")
    bits_check = ss.find("if (!residual_ok)")
    if not (0 <= bits_check < stamp):
        fails.append("AC2: residual_ok check must precede ticket stamp")

    # AC3 — production fail-closed
    must("RejectHard", "AC3", ss)
    must("StealSafetyDecision::Ok", "AC3", ss)
    if "soft path can't override" not in ss and "no soft-mode" not in ss:
        fails.append("AC3: soft/production fail-closed documentation missing")

    # AC4 — query
    must("schema-2929", "AC4", qjit)
    must("steal-invariant-table-wired", "AC4", qjit)
    must("steal-invariant-last-reject-bits", "AC4", qjit)
    must("steal-invariant-boundary-fail-total", "AC4", qjit)
    must("steal-invariant-snapshot-fail-total", "AC4", qjit)
    must("schema-2721", "AC4 lineage", qjit)
    must("schema-2699", "AC4 lineage", qjit)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC4 residual preserved", hdr)
    must("g_steal_safety_transaction_ok_total", "AC4 txn preserved", hdr)

    # AC5/AC6 — tests + linter + no invent/design
    must("ac2929_1_invariant_table_and_counters", "AC5", test)
    must("ac2929_2_ticket_only_after_all_invariants", "AC5", test)
    must("ac2929_3_soft_metric_only_production_fail_closed", "AC5", test)
    must("ac2929_4_query_additive", "AC5", test)
    must("ac2929_5_source_and_linter", "AC5", test)
    must("check_steal_invariant_table_2929", "AC5", build)
    must("ac2844_1_sole_enqueue_gate", "AC5 #2844 preserved", test)
    must("ac2721_1_residual_hard_and_inside_transaction", "AC5 #2721 preserved", test)
    if (ROOT / "tests" / "serve" / "test_issue_2929.cpp").is_file():
        fails.append("AC5: test_issue_2929.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2929*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2929 StealInvariant table — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

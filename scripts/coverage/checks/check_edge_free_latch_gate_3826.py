#!/usr/bin/env python3
"""Issue #3826: edge-free MutationHold under multi-worker latch is gated.

Cross-fiber cancel/force_safepoint cannot unlock foreign unique_lock.
Under production multi-worker latch, peer poll of an edge-free holder
past budget arms Ready residual sticky (#3619/#3288) and/or join
Reclaimed (#3764); same-fiber consume → depth0/!held. Soft metric-only.

Contract (one row per AC):
  AC1  Peer busy/inbody poll under latch arms residual sticky when
       no-edge still held; foreign force_release only re-arms cancel
  AC2  Oracles ac3254_2 / ac3325_2 / ac3764_2 retained (never foreign unlock)
  AC3  Soft / !reject_enabled early-return unchanged
  AC4  Tests extend mailbox_hold_starvation_hard; stamp; no invent/docs

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

    fc = _read("src/serve/fiber.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    must("Issue #3826", "AC1 fiber cite", fc)
    must("steal_safety_production_residual_zero_v_read", "AC1 sticky arm", fc)
    must("kMutationHoldBudgetEdgeFreeLatchGateIssue = 3826", "AC1 stamp", mhb)

    busy = fc.find("aura_hold_budget_poll_busy_path(void) noexcept")
    busy_win = fc[busy : busy + 2800] if busy >= 0 else ""
    must("Issue #3826", "AC1 busy cite", busy_win)
    must("steal_safety_production_residual_zero_v_read", "AC1 busy sticky", busy_win)
    must("dispose_no_edge_holder", "AC1 busy dispose", busy_win)

    rel = emb.find("aura_evaluator_force_release_outermost_holder")
    rel_win = emb[rel : rel + 1800] if rel >= 0 else ""
    must("Issue #3826", "AC1 emb cite", rel_win)
    must("aura_fiber_request_hold_budget_cancel", "AC1 foreign cancel-only", rel_win)
    must("cur->id() == fiber_id", "AC1 same-fiber unlock only", rel_win)

    # AC2: retained oracles (never foreign unlock)
    syn = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    must("ac3254_2_cross_fiber_no_preemptive_unlock", "AC2 oracle 3254", syn)
    must("ac3325_2_cross_fiber_idle_poll_no_foreign_unique_lock", "AC2 oracle 3325", syn)
    must("ac3764_2_no_edge_holder_disposed", "AC2 oracle 3764", t)

    must("mutation_hold_budget_reject_enabled()", "AC3 Soft gate busy", busy_win)
    must("return 0; // Soft / sandbox=off", "AC3 Soft inbody", fc)

    must("ac3826_1_edge_free_peer_poll_gate", "AC4 AC1 test", t)
    must("ac3826_2_defer_clear_after_body", "AC4 AC2 test", t)
    must("ac3826_3_soft_and_source", "AC4 AC3 test", t)
    must("Never mid-mutation steal Ok", "AC4 steal Ok guard", t)
    must("mutation_hold_defer_active()==0 after body exit", "AC4 defer AC", t)
    must("check_edge_free_latch_gate_3826", "AC4 build.py", build)

    if (ROOT / "tests" / "serve" / "test_issue_3826.cpp").is_file():
        fails.append("AC4: test_issue_3826.cpp present (forbidden invent)")
    if (ROOT / "tests" / "issues" / "test_issue_3826.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3826.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3826-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3826 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3826 edge-free latch gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

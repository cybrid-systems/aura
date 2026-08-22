#!/usr/bin/env python3
"""Issue #3254: non-cooperative outermost body past 2×SLO force-edge.

#3222/#3223 still required the holder to hit check_gc_safepoint / yield.
#3254: runtime injects a synthetic MutationBoundary yield and consumes it
on the holder (dual restore + unlock + depth 0). Cross-fiber never drops
unique_lock. Soft: observe-only. Reuse forced_unlock_total +
forced_fail_closed_total.

Contract:
  AC1 Production poll past bound injects+consumes on holder (no accidental
      check_gc_safepoint required)
  AC2 Cross-fiber force_degrade + urgent poll; foreign thread never unlocks
  AC3 Soft / !reject_enabled: metric-only
  AC4 Force path abort_restore / exit_mutation_boundary(false) then unlock
  AC5 Extend test_hold_budget_synthetic_yield_injection; no invent
  AC6 Source-cite linter + build.py; no docs/design/3254-*

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
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    poll_win = fc[poll_pos : poll_pos + 8000] if poll_pos >= 0 else ""

    must("kMutationHoldBudgetNoncoopForceEdgeIssue", "AC1 stamp", mhb)
    must("Issue #3254", "AC1 poll cite", poll_win)
    must("inject_synthetic_mutation_boundary_yield", "AC1 inject", poll_win)
    must("aura_evaluator_force_release_outermost_holder", "AC1 consume", poll_win)
    must("ac3254_1_same_fiber_poll_consumes_injected_edge", "AC1 test", t)

    must("cur->id() == fid", "AC2 same-fiber inject", poll_win)
    must("aura_evaluator_force_degrade_outermost_holder", "AC2 degrade", efm)
    if "aura_evaluator_force_unlock_outermost_holder" in poll_win:
        fails.append("AC2: poll spells force_unlock (breaks #3160 AC12)")
    must("ac3254_2_cross_fiber_no_preemptive_unlock", "AC2 test", t)

    must("return 0; // Soft / sandbox=off", "AC3 poll Soft", poll_win)
    must("ac3254_3_soft_observe_only", "AC3 test", t)

    must("exit_mutation_boundary(false)", "AC4 dual restore", emb)
    must("force_release_hold_budget_inbody", "AC4 helper", emb)
    must("if (!inbody_force_exited_)", "AC4 no double-exit", emb)
    must("ac3254_4_topology_dual_restore", "AC4 test", t)

    if "g_3254_" in fc or "g_3254_" in mhb or "g_3254_" in emb:
        fails.append("AC4: new g_3254_* counter (reuse existing)")

    must("run_test_hold_budget_noncoop_force_edge", "AC5", t)
    must("check_hold_budget_noncoop_force_edge_3254", "AC6 build.py", build)

    if (ROOT / "tests" / "issues" / "test_issue_3254.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3254.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3254.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3254.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3254-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3254 hold_budget_noncoop_force_edge:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3254 hold_budget_noncoop_force_edge: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

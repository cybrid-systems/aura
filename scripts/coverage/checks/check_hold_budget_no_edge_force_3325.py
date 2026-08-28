#!/usr/bin/env python3
"""Issue #3325: residual outermost hold past 2×SLO with no cooperative edge.

#3254/#3285/#3071 still require a same-fiber poll / injected edge consume.
#3325: scheduler idle / worker park under production_multi_worker_latched
re-injects synthetic MutationBoundary yield + force_safepoint on the live
holder past 2×SLO and bumps hold_budget_no_edge_force_total. Same-fiber
consume still via force_release (unique_lock owner). Cross-fiber never
drops unique_lock. Soft: metric-only.

Contract:
  AC1 Production latched poll past bound injects+consumes on holder
      (no accidental check_gc_safepoint)
  AC2 Cross-fiber idle/park re-inject; foreign thread never drops unique_lock
  AC3 Soft / !reject_enabled: metric-only
  AC4 Existing forced_unlock_total + forced_fail_closed_total + new
      hold_budget_no_edge_force_total
  AC5 Extend test_hold_budget_synthetic_yield_injection; no invent
  AC6 Source-cite linter + build.py; no docs/design/3325-*

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
    wc = _read("src/serve/worker.cpp")
    sc = _read("src/serve/scheduler.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    poll_win = fc[poll_pos : poll_pos + 9000] if poll_pos >= 0 else ""

    must("kMutationHoldBudgetNoEdgeForceIssue", "AC1 stamp", mhb)
    must("Issue #3325", "AC1 poll cite", poll_win)
    must("g_production_multi_worker_latched", "AC1 latch gate", poll_win)
    must("inject_synthetic_mutation_boundary_yield", "AC1 inject", poll_win)
    must("aura_evaluator_force_release_outermost_holder", "AC1 consume", poll_win)
    must("ac3325_1_same_fiber_poll_consumes_without_natural_edge", "AC1 test", t)

    must("aura_hold_budget_poll_inbody_window()", "AC2 worker park poll", wc)
    must("aura_hold_budget_poll_inbody_window()", "AC2 scheduler idle poll", sc)
    must("cur->id() == fid", "AC2 same-fiber inject", poll_win)
    if "aura_evaluator_force_unlock_outermost_holder" in poll_win:
        fails.append("AC2: poll spells force_unlock (breaks #3160 AC12)")
    must("ac3325_2_cross_fiber_idle_poll_no_foreign_unique_lock", "AC2 test", t)

    must("return 0; // Soft / sandbox=off", "AC3 poll Soft", poll_win)
    must("ac3325_3_soft_observe_only", "AC3 test", t)

    must("g_mutation_hold_budget_forced_unlock_total", "AC4 unlock", mhb)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC4 fail-closed", mhb)
    must("g_hold_budget_no_edge_force_total", "AC4 no-edge total", mhb)
    must("g_hold_budget_no_edge_force_total.fetch_add", "AC4 poll bumps no-edge", poll_win)

    must("run_test_hold_budget_no_edge_force_3325", "AC5", t)
    must("aura_hold_budget_poll_inbody_window", "AC5 chaos poll", chaos)
    must("check_hold_budget_no_edge_force_3325", "AC6 build.py", build)

    if (ROOT / "tests" / "issues" / "test_issue_3325.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3325.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3325.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3325.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3325-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3325 hold_budget_no_edge_force:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3325 hold_budget_no_edge_force: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

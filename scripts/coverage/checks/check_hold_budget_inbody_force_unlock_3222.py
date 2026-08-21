#!/usr/bin/env python3
"""Issue #3222: non-yield outermost body hold-budget force-unlock.

#3194 same-fiber force-release only ran when the holder called poll.
Scheduler idle poll is always cross-fiber (pending-cancel only). #3222
wires inbody poll onto Fiber::check_gc_safepoint so a live same-fiber
body past 2×SLO force-releases workspace hold + depth (reuse #3194 /
#3118 / #3035) and marks failed. Cross-fiber pending-cancel only.
Soft: metric-only. Reuse forced_unlock_total + forced_fail_closed_total.

Contract:
  AC1 Production check_gc_safepoint past bound → same-fiber force-release
      + mark failed (poll inbody window on the holder)
  AC2 Cross-fiber pending-cancel only; poll cannot spell force_unlock
      (#3160 AC12); force_unlock ABI aliases force_release
  AC3 Soft / sandbox=off / !reject_enabled: zero behavioural change
  AC4 No new counters; reuse forced_unlock_total + forced_fail_closed_total
  AC5 Extend test_hold_budget_synthetic_yield_injection; no invent
  AC6 Source-cite linter + build.py; no docs/design/3222-*

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
    fh = _read("src/serve/fiber.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    br = _read("src/compiler/fiber_bridge.cpp")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    sp = fc.find("void Fiber::check_gc_safepoint()")
    sp_win = fc[sp : sp + 2500] if sp >= 0 else ""
    poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    poll_win = fc[poll_pos : poll_pos + 7000] if poll_pos >= 0 else ""

    # AC1
    must("Issue #3222", "AC1 safepoint cite", sp_win)
    must("aura_hold_budget_poll_inbody_window()", "AC1 safepoint poll", sp_win)
    must("kMutationHoldBudgetInbodyForceUnlockIssue", "AC1 stamp", mhb)
    must("aura_evaluator_force_unlock_outermost_holder", "AC1 ABI", fh)
    must("aura_evaluator_force_unlock_outermost_holder", "AC1 helper", emb)

    # AC2
    must("cur->id() == fiber_id", "AC2 same-fiber", emb)
    must("aura_fiber_request_hold_budget_cancel", "AC2 cross-fiber cancel", emb)
    must("aura_evaluator_force_release_outermost_holder(fiber_id)", "AC2 alias reuses 3194", emb)
    if "aura_evaluator_force_unlock_outermost_holder" in poll_win:
        fails.append("AC2: poll spells force_unlock (breaks #3160 AC12 unlock substring)")
    must("aura_evaluator_force_release_outermost_holder", "AC2 poll still force_release", poll_win)
    must("ac3222_2_cross_fiber_no_preemptive_unlock", "AC2 test", t)

    # AC3
    must("if (!mutation_hold_budget_reject_enabled())", "AC3 helper gate", emb)
    must("return 0; // Soft / sandbox=off", "AC3 poll Soft", poll_win)
    must("ac3222_3_soft_observe_only", "AC3 test", t)

    # AC4
    must("g_mutation_hold_budget_forced_unlock_total", "AC4", emb)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC4", emb)
    if "g_3222_" in fc or "g_3222_" in mhb or "g_3222_" in emb:
        fails.append("AC4: new g_3222_* counter (reuse existing)")

    # AC5 / AC6
    must("ac3222_1_same_fiber_safepoint_force_unlock", "AC5", t)
    must("run_test_hold_budget_inbody_force_unlock", "AC5", t)
    must("check_hold_budget_inbody_force_unlock_3222", "AC6 build.py", build)
    must("aura_evaluator_force_unlock_outermost_holder", "AC6 weak stub", br)

    if (ROOT / "tests" / "issues" / "test_issue_3222.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3222.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3222.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3222.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3222-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3222 hold_budget_inbody_force_unlock:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3222 hold_budget_inbody_force_unlock: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3223: cross-fiber hold-budget cancel must nudge victim inbody poll.

force_degrade on a foreign worker used to set pending-cancel only; the
victim ignored cancel until it itself polled. #3223 wires
aura_fiber_request_urgent_inbody_poll so the holder's check_gc_safepoint
runs the same force-release as same-fiber (#3222). Foreign thread never
unlocks unique_lock. Soft: observe-only. Reuse cross-fiber fired /
consumed + forced_unlock_total.

Contract:
  AC1 Production cross-fiber force_degrade → urgent inbody poll + victim
      check_gc_safepoint force-release past bound
  AC2 Soft / sandbox=off: zero behavioural change
  AC3 Nested Guards never independently force (outermost TLS Guard)
  AC4 No new counters; reuse cross-fiber fired/consumed + forced_unlock
  AC5 Extend test_hold_budget_synthetic_yield_injection; no invent
  AC6 Source-cite linter + build.py; no docs/design/3223-*

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
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    br = _read("src/compiler/fiber_bridge.cpp")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    sp = fc.find("void Fiber::check_gc_safepoint()")
    sp_win = fc[sp : sp + 2800] if sp >= 0 else ""
    deg = efm.find("aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id)")
    deg_win = efm[deg : deg + 5000] if deg >= 0 else ""

    # AC1
    must("Issue #3223", "AC1 stamp cite", mhb)
    must("kMutationHoldBudgetCrossFiberUrgentInbodyPollIssue", "AC1 stamp", mhb)
    must("aura_fiber_request_urgent_inbody_poll", "AC1 ABI", fh)
    must("aura_fiber_request_urgent_inbody_poll", "AC1 impl", fc)
    must("aura_fiber_request_urgent_inbody_poll(fiber_id)", "AC1 force_degrade", deg_win)
    must("inject_synthetic_mutation_boundary_yield", "AC1 inject", fc)
    must("peek_urgent_inbody_poll", "AC1 safepoint", sp_win)
    must("aura_hold_budget_poll_inbody_window()", "AC1 safepoint poll", sp_win)
    must("Issue #3223", "AC1 safepoint cite", sp_win)

    # AC2
    must("mutation_hold_budget_reject_enabled()", "AC2 gate", deg_win)
    must("ac3223_2_soft_observe_only", "AC2 test", t)

    # AC3
    must("g_tls_outermost_guard", "AC3 outermost", emb)
    must("cur->id() == fiber_id", "AC3 same-fiber", emb)

    # AC4
    must("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total", "AC4 fired", efm)
    must("g_mutation_hold_budget_forced_unlock_total", "AC4 unlock", emb)
    if "g_3223_" in fc or "g_3223_" in mhb or "g_3223_" in efm:
        fails.append("AC4: new g_3223_* counter (reuse existing)")

    # AC5 / AC6
    must("ac3223_1_cross_fiber_force_degrade_victim_force_release", "AC5", t)
    must("run_test_hold_budget_cross_fiber_urgent_inbody_poll", "AC5", t)
    must("check_cross_fiber_urgent_inbody_poll_3223", "AC6 build.py", build)
    must("aura_fiber_request_urgent_inbody_poll", "AC6 weak stub", br)

    if (ROOT / "tests" / "issues" / "test_issue_3223.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3223.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3223.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3223.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3223-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3223 cross_fiber_urgent_inbody_poll:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3223 cross_fiber_urgent_inbody_poll: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

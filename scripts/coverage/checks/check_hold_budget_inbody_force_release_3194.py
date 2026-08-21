#!/usr/bin/env python3
"""Issue #3194: non-cooperative hold-budget inbody force-release.

After 2× inbody window, poll must force-release workspace hold + depth
on the same-fiber path (reuse #3118/#3035). Cross-fiber only re-arms
pending-cancel (no preemptive mutex drop). Soft: metric-only.

Contract:
  AC1 Production poll past bound → same-fiber force-release + mark failed
  AC2 Cross-fiber pending-cancel only; same-fiber may release immediately
  AC3 Soft / sandbox=off / !reject_enabled: zero behavioural change
  AC4 No new counters; reuse forced_unlock_total + forced_fail_closed_total
  AC5 Extend test_hold_budget_synthetic_yield_injection; no invent
  AC6 Source-cite linter + build.py; no docs/design/3194-*

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
    br = _read("src/compiler/fiber_bridge.cpp")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept")
    poll_win = fc[poll_pos : poll_pos + 7000] if poll_pos >= 0 else ""

    # AC1
    must("aura_evaluator_force_release_outermost_holder", "AC1 poll", poll_win)
    must("Issue #3194", "AC1 poll cite", poll_win)
    must("force_release_hold_after_cancel_", "AC1 reuse 3118", emb)
    must("g_tls_outermost_guard", "AC1 TLS Guard", emb)
    must("kMutationHoldBudgetInbodyForceReleaseIssue", "AC1 stamp", mhb)

    # AC2
    must("cur->id() == fiber_id", "AC2 same-fiber", emb)
    must("aura_fiber_request_hold_budget_cancel", "AC2 cross-fiber cancel", emb)
    must("ac3194_2_cross_fiber_no_preemptive_release", "AC2 test", t)

    # AC3
    must("if (!mutation_hold_budget_reject_enabled())", "AC3 helper gate", emb)
    must("return 0; // Soft / sandbox=off", "AC3 poll Soft", poll_win)
    must("ac3194_3_soft_observe_only", "AC3 test", t)

    # AC4
    must("g_mutation_hold_budget_forced_unlock_total", "AC4", emb)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC4", emb)
    if "g_3194_" in fc or "g_3194_" in mhb or "g_3194_" in emb:
        fails.append("AC4: new g_3194_* counter (reuse existing)")

    # AC5 / AC6
    must("ac3194_1_same_fiber_force_release", "AC5", t)
    must("run_test_hold_budget_inbody_force_release", "AC5", t)
    must("check_hold_budget_inbody_force_release_3194", "AC6 build.py", build)
    must("aura_evaluator_force_release_outermost_holder", "AC6 weak stub", br)

    if (ROOT / "tests" / "issues" / "test_issue_3194.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3194.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3194.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3194.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3194-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3194 hold_budget_inbody_force_release:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3194 hold_budget_inbody_force_release: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

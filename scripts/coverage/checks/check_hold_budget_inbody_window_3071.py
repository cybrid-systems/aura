#!/usr/bin/env python3
"""Issue #3071: in-body non-poll window after hold-budget cancel.

Residual of #3035/#2999/#2932: cancel is armed and dtor is fail-closed,
but a body that never reaches check_gc_safepoint / yield / Phase-5 still
holds workspace_mtx_ until it happens to exit. #3071 stamps cancel-arm
time and lets the scheduler idle path poll: if the holder is still
outermost-held past a bounded multiple of the hold SLO, bump
inbody-window-exceeded and re-arm force-safepoint via existing
aura_evaluator_force_degrade_outermost_holder (no preemptive unlock).

Contract (one row per AC):
  AC1 Production: after cancel arm + elapsed > bound + holder still
     live → exceeded + escalate force-degrade / re-arm force-safepoint.
     Soft: observe only.
  AC2 No preemptive workspace_mtx_ unlock while body still runs.
  AC3 Nested never independently force (outermost live snapshot).
  AC4 Additive hold-budget-inbody-window-exceeded-total; schema-3071;
     #3035/#2999/#2932 preserved.
  AC5 Extend hold-starvation / chaos residual_zero (#81967); linter.
  AC6 Source-cite + linter; no docs/design/* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    fc = _read("src/serve/fiber.cpp")
    fh = _read("src/serve/fiber.h")
    sc = _read("src/serve/scheduler.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = read_query_prims()
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    must("Issue #3071", "AC1", mhb)
    must("kMutationHoldBudgetInbodyWindowIssue = 3071", "AC1", mhb)
    must("aura_hold_budget_poll_inbody_window", "AC1 poll", fc)
    must("aura_evaluator_force_degrade_outermost_holder", "AC1 escalate", fc)
    must("aura_hold_budget_poll_inbody_window", "AC1 ABI", fh)
    must("aura_hold_budget_poll_inbody_window", "AC1 scheduler poll", sc)
    must("aura_hold_budget_cancel_armed", "AC1 scheduler shrink", sc)
    must("mutation_hold_budget_note_cancel_armed", "AC1 stamp", fc)
    must("mutation_hold_budget_note_cancel_consumed", "AC1 clear", fc)

    must("no workspace_mtx_ unlock", "AC2 no unlock comment", fc)
    if "workspace_mtx_.unlock" in fc:
        fails.append("AC2: fiber.cpp contains workspace_mtx_.unlock (forbidden)")
    must("mutation_hold_budget_reject_enabled()", "AC2 Soft gate", fc)

    must("is_outermost_", "AC3 outermost-only dtor", emb)
    must("mutation_hold_live_snapshot", "AC3 poll uses live holder", fc)

    must_key("hold-budget-inbody-window-exceeded-total", "AC4", q)
    must_key("mutation-hold-budget-inbody-window-exceeded-total", "AC4", q)
    must_key("mutation-hold-budget-inbody-window-wired", "AC4", q)
    must_key("schema-3071", "AC4", q)
    must_key("issue-3071", "AC4", q)
    must_key("schema-3035", "AC4 preserved", q)
    must_key("schema-2999", "AC4 preserved", q)
    must_key("schema-2932", "AC4 preserved", q)
    must_key("schema-2726", "AC4 preserved", q)

    must("ac3071_1_production_inbody_window", "AC5", t)
    must("ac3071_2_no_unlock_soft_observe", "AC5", t)
    must("ac3071_3_nested_and_happy", "AC5", t)
    must("ac3071_4_query_keys", "AC5", t)
    must("ac3071_residual_inbody_window_cite", "AC5 chaos residual_zero", chaos)
    must("max hold-after-cancel exceeds inbody bound", "AC5 soak fail-closed", chaos)
    must("check_hold_budget_inbody_window_3071", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_3071.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3071.cpp present (forbidden invent)")

    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("3071-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3071 hold-budget in-body window — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

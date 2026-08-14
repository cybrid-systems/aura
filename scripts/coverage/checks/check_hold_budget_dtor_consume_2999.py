#!/usr/bin/env python3
"""Issue #2999: outermost Guard dtor consume of hold-budget cancel.

Residual of #2932: fail-closed must not depend on check_gc_safepoint
to start the exit sequence. In-body window (body that never exits)
still requires #2932 force-safepoint to *enter* dtor — do not claim
that window is gone.

Contract (one row per AC):
  AC1 Production: outermost dtor with pending cancel fail-closes and
     releases workspace_mtx_ / MutationHold / residual (#2846) even if
     check_gc_safepoint never ran.
  AC2 Soft / sandbox=off: observe only; no consume / no forced fail.
  AC3 Nested guards never independently consume (outermost-only).
  AC4 Remaining in-body window documented; #2932 safepoint path kept.
  AC5 Additive: forced-fail-closed + dtor-consume-total; schema-2999;
     extend hold-starvation / chaos residual_zero (#81967).
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
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fc = _read("src/serve/fiber.cpp")
    q = read_query_prims()
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    must("Issue #2999", "AC1", emb)
    must("consume_hold_budget_cancel", "AC1", emb)
    must("forced_fail_closed_dtor_consume_total", "AC1", emb)
    must("forced_fail_closed_total", "AC1 dtor bumps 2932 total", emb)
    must("kMutationHoldBudgetForcedFailClosedDtorIssue = 2999", "AC1", mhb)
    must("in-body window", "AC1 remaining window", mhb)

    must("peek_hold_budget_cancel", "AC2 Soft peek", emb)
    must("soft_observe_total", "AC2 Soft observe", emb)
    must("mutation_hold_budget_reject_enabled()", "AC2", emb)

    must("is_outermost_", "AC3", emb)
    must("Nested", "AC3", mhb)

    must("aura_evaluator_try_hold_budget_fail_closed_at_safepoint", "AC4 safepoint kept", efm)
    must("Issue #2999", "AC4", efm)
    must("Issue #2999", "AC4 fiber.cpp", fc)
    must("preemptive unlock", "AC4 no preemptive unlock", emb)
    must("enter dtor", "AC4 in-body must enter dtor", emb)

    must_key("mutation-hold-budget-forced-fail-closed-dtor-consume-total", "AC5", q)
    must_key("schema-2999", "AC5", q)
    must_key("issue-2999", "AC5", q)
    must_key("schema-2932", "AC5 preserved", q)
    must_key("mutation-hold-budget-forced-fail-closed-total", "AC5 preserved", q)
    must("ac2999_1_dtor_consume_fail_closed", "AC5", t)
    must("ac2999_2_soft_observe_only", "AC5", t)
    must("ac2999_3_nested_outermost_only", "AC5", t)
    must("ac2999_residual_dtor_consume_cite", "AC5 chaos residual_zero", chaos)
    must("check_hold_budget_dtor_consume_2999", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_2999.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_2999.cpp present (forbidden invent)")

    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2999-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2999 hold-budget dtor consume — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

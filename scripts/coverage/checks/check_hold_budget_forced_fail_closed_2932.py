#!/usr/bin/env python3
"""Issue #2932: hold-budget overtime → forced outermost fail-closed
(not cooperative Phase-5-only cancel).

Contract (one row per AC):
  AC1 Under production defaults, hold-budget overtime forces outermost
     mutation failure and releases workspace_mtx_ / MutationHold even when
     the body is a non-yielding loop (via force-safepoint + consume path
     at check_gc_safepoint / yield).
  AC2 Soft / sandbox=off: metric-only unless explicit hard env; no forced fail.
  AC3 Nested guards never independently force-fail (outermost-only).
  AC4 Residual closed-loop (#2846) still runs on the forced-failure exit path.
  AC5 Additive observability (forced-fail-closed total + wired + schema-2932);
     source-cite + tests in src-aligned suite (extend mailbox hold starvation).
  AC6 No docs/design/* per #1655.

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
    fh = _read("src/serve/fiber.h")
    fbr = _read("src/compiler/fiber_bridge.cpp")
    q = read_query_prims()
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — force-safepoint paired with cancel + safepoint fail-closed ABI.
    must("Issue #2932", "AC1", fc)
    must("request_force_safepoint", "AC1 cancel pairs force", fc)
    must("aura_evaluator_try_hold_budget_fail_closed_at_safepoint", "AC1", fc)
    must("peek_hold_budget_cancel", "AC1", fc)
    must("Issue #2932", "AC1", efm)
    must("aura_evaluator_try_hold_budget_fail_closed_at_safepoint", "AC1", efm)
    must("mark_outermost_mutation_failed", "AC1", efm)
    must("consume_hold_budget_cancel", "AC1", efm)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC1", efm)
    must("Issue #2932", "AC1", mhb)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC1", mhb)
    must("kMutationHoldBudgetForcedFailClosedIssue = 2932", "AC1", mhb)
    must("request_force_safepoint", "AC1 fiber.h cite", fh)
    # Force-degrade still wires cancel (which now pairs force-safepoint).
    must("aura_fiber_request_hold_budget_cancel", "AC1 force-degrade", efm)

    # AC2 — Soft / reject_enabled gate.
    must("mutation_hold_budget_reject_enabled()", "AC2", efm)
    must("Soft / sandbox=off", "AC2", efm)
    must("mutation_hold_budget_reject_enabled()", "AC2 Phase-5", emb)

    # AC3 — outermost-only (nested never independently force-fail).
    must("outermost", "AC3", efm)
    must("is_outermost_", "AC3", emb)
    must("Nested", "AC3", mhb)

    # AC4 — residual #2846 on forced-failure exit path (Phase-5 failure).
    must("Issue #2846", "AC4", emb)
    must("close_residual_defer_after_exit", "AC4", emb)
    must("Issue #2932", "AC4 Phase-5 cite", emb)
    must("forced-failure", "AC4 residual on forced", emb)

    # AC5 — observability + tests + linter wire.
    must_key("mutation-hold-budget-forced-fail-closed-total", "AC5", q)
    must_key("mutation-hold-budget-forced-fail-closed-wired", "AC5", q)
    must_key("schema-2932", "AC5", q)
    must_key("issue-2932", "AC5", q)
    # Prior surfaces preserved (strict superset).
    must_key("schema-2726", "AC5 preserved", q)
    must_key("schema-2701", "AC5 preserved", q)
    must("ac2932_1_force_safepoint_fail_closed", "AC5", t)
    must("ac2932_2_soft_metric_only", "AC5", t)
    must("ac2932_3_nested_outermost_only", "AC5", t)
    must("ac2932_4_residual_closed_loop", "AC5", t)
    must("ac2932_5_source_and_linter", "AC5", t)
    must("ac2932_6_no_docs_design", "AC5", t)
    must("check_hold_budget_forced_fail_closed_2932", "AC5", build)
    must("aura_evaluator_try_hold_budget_fail_closed_at_safepoint", "AC5 weak", fbr)
    if (ROOT / "tests" / "serve" / "test_issue_2932.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_2932.cpp present (forbidden invent)")

    # AC6 — no docs/design.
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2932-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2932 hold-budget forced fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

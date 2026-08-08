#!/usr/bin/env python3
"""Issue #2701: mutation hold-budget timeout → force degrade / reject new mutate admit.

Contract (one row per AC):
  AC1 src/compiler/mutation_hold_budget.h defines the budget accessor +
     live snapshot probe + the #2701 budget-reject counter + production/
     hard-env decision (mutation_hold_budget_reject_enabled). The
     evaluator_mutation_boundary.cpp try_acquire + try_acquire_for_region
     paths consult the budget BEFORE the #2630/#2660 security-schedule
     gate (order: #2587 mailbox-hold-starvation → #2701 budget → #2630/
     #2660 security-schedule).
  AC2 Soft / sandbox / test override: metric-only (counter bump, no
     reject) unless AURA_MUTATION_HOLD_BUDGET_HARD=1.
  AC3 Agent-visible counter: mutation_hold_budget_reject_total +
     mutation_hold_budget_soft_observe_total + mutation_hold_budget_wired
     sentinel + schema-2701 / issue-2701 in evaluator_primitives_query.cpp.
  AC4 Order with #2660 security-schedule: budget BEFORE schedule (locked
     in source — both gates observable; documented in source comments).
  AC5 Coverage linter + unit extension of test_mailbox_hold_starvation_hard
     (or successor) per #81967.
  AC6 No docs/design/ per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    def must_key(n: str, label: str, hay: str) -> None:
        # clang-format may split adjacent string literals; strip quotes +
        # whitespace so "mutation-hold-budget-" "reject-total" still matches.
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — budget accessor + reject counter + decision helper
    must("Issue #2701", "AC1", mhb)
    must("g_mutation_hold_budget_reject_total", "AC1", mhb)
    must("g_mutation_hold_budget_soft_observe_total", "AC1", mhb)
    must("g_mutation_hold_budget_wired", "AC1", mhb)
    must("kMutationHoldBudgetRejectIssue = 2701", "AC1", mhb)
    must("mutation_hold_budget_reject_total_v_read", "AC1", mhb)
    must("mutation_hold_budget_check", "AC1", mhb)
    must("mutation_hold_budget_reject_enabled", "AC1", mhb)
    must("AdmissionRejected: mutation-hold-budget", "AC1", emb)
    must("mutation_hold_budget_check()", "AC1", emb)

    # AC2 — Soft / sandbox metric-only
    must("AURA_MUTATION_HOLD_BUDGET_HARD", "AC2", mhb)
    must("mutation_hold_budget_hard_env", "AC2", mhb)
    must("Soft path", "AC2", emb)

    # AC3 — additive query keys (format-robust: clang-format may split keys)
    must_key("query:mutation-hold-budget-gate", "AC3", q)
    must_key("mutation-hold-budget-reject-total", "AC3", q)
    must_key("mutation-hold-budget-soft-observe-total", "AC3", q)
    must_key("mutation-hold-budget-wired", "AC3", q)
    must_key("schema-2701", "AC3", q)
    must_key("issue-2701", "AC3", q)

    # AC4 — order with #2660 security-schedule (budget BEFORE schedule)
    must("#2587 mailbox-hold-starvation", "AC4", emb)
    must("#2701 budget", "AC4", emb)
    must("#2630/#2660 security-schedule", "AC4", emb)

    # AC5 — test extension per #81967
    must("ac2701_1_budget_reject_production", "AC5", t)
    must("ac2701_2_soft_path_metric_only", "AC5", t)
    must("ac2701_3_order_with_security_schedule", "AC5", t)
    must("ac2701_4_query_keys_added", "AC5", t)
    must("ac2701_5_source_and_linter", "AC5", t)
    must("ac2701_6_no_docs_design", "AC5", t)
    must("check_mutation_hold_budget_reject_2701", "AC5", build)

    # AC6 — no docs/design/2701-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2701-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2700 + #2699 + #2693 linters still green
    for prev in (
        "check_handoff_ref_mailbox_gate_2700.py",
        "check_steal_safety_transaction_2699.py",
        "check_joint_epoch_bump_coverage.py",
    ):
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "coverage" / "checks" / prev),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2701 mutation hold-budget reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

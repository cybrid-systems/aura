#!/usr/bin/env python3
"""Issue #3485: mailbox p99/SLO live unions into mutation_hold_budget_check.

#2947 denies new mutate on mailbox_hold_slo. #2701 budget check only
looked at hold duration_us. Holder body cannot yield (#2200) so duration
can stay under hold SLO while p99 ≥ mailbox SLO — senders backpressure,
new admit denied, holder keeps workspace_mtx_.

Fix: OR #3002 mailbox_hold_slo_live_signal into the existing budget
check when snap.held. try_acquire over_budget arm already degrades the
holder. Soft observe-only. No hist walk on p99==0. No new query key.

Contract:
  AC1  held && (duration over OR mailbox SLO live) → over_budget
  AC2  Soft observe-only (reject_enabled false)
  AC3  no new query key; reuse p99 + hold-budget totals
  AC4  p99==0: one extra relaxed load, no hist walk
  AC5  extend test_mailbox_hold_starvation_hard
  AC6  source-cite mutation_hold_budget_check + mailbox_hold_slo_live_signal

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    fc = _read("src/serve/fiber.cpp")
    mb = _read("src/serve/multi_fiber_mailbox.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    chk = mhb.find("inline MutationHoldBudgetCheck mutation_hold_budget_check()")
    cwin = mhb[chk : chk + 1800] if chk >= 0 else ""
    must("Issue #3485", "AC1 check cite", cwin)
    must("mutation_hold_mailbox_slo_live()", "AC1 mailbox union", cwin)
    must("mailbox_hold_slo_live_signal", "AC6 SSOT cite in check", cwin)
    must("snap.held", "AC1 held gate", cwin)
    must("g_mutation_hold_budget_reject_total", "AC1 reject counter", cwin)
    must("g_mutation_hold_budget_soft_observe_total", "AC2 soft observe", cwin)
    must("mutation_hold_budget_reject_enabled()", "AC2 reject_enabled", cwin)

    must("bool mutation_hold_mailbox_slo_live()", "AC6 helper decl", mhb)
    hpos = fc.find("bool mutation_hold_mailbox_slo_live()")
    hwin = fc[max(0, hpos - 500) : hpos + 1600] if hpos >= 0 else ""
    must("Issue #3485", "AC6 helper cite", hwin)
    must("mailbox_hold_slo_live_signal", "AC6 SSOT call", hwin)
    must("sample_mailbox_hold_slo_live", "AC6 sample", hwin)
    must("mailbox_under_boundary_wait_us_p99", "AC4 p99 load", hwin)
    must("if (p99 == 0)", "AC4 p99==0 short-circuit", hwin)
    must_not("note_mailbox_under_boundary_wait_sample", "AC4 no hist walk", hwin)

    must("mailbox_hold_slo_live_signal", "AC6 SSOT stays in mailbox", mb)
    must("mutation_hold_budget_check()", "AC1 try_acquire consults", emb)
    must("aura_evaluator_force_degrade_outermost_holder", "AC1 degrade stays", emb)

    must("3485 AC1: p99 hot + held → over_budget", "AC5 runtime", t)
    must("3485 AC2: Soft does not reject", "AC5 Soft", t)
    must("3485 AC4: p99==0 + short hold is not over-budget", "AC4 quiet", t)
    must("ac3485_1_p99_hot_held_over_budget", "AC5 fn", t)

    must("check_mailbox_hold_budget_p99_union_3485", "AC6 build.py", build)
    prev = build.find("check_mailbox_hold_slo_ssot_soak_3002")
    ours = build.find("check_mailbox_hold_budget_p99_union_3485")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3002")

    must_not("schema-3485", "AC3 no schema-3485", mhb + emb)
    must_not("g_3485_", "AC3 no g_3485_*", mhb + fc)
    must_not("query:mailbox-hold-budget-p99", "AC3 no new query", mhb)
    if (ROOT / "tests" / "serve" / "test_issue_3485.cpp").is_file():
        fails.append("AC6: test_issue_3485.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3485.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3485.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3485-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3485 mailbox_hold_budget_p99_union:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3485 mailbox_hold_budget_p99_union: p99 unions into hold-budget; Soft observe")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3825: safepoint fail-closed must force-release hold, not mark_failed-only.

aura_evaluator_try_hold_budget_fail_closed_at_safepoint used to consume
cancel + mark_failed() only — workspace_mtx_ + MutationHold stayed held
until Guard dtor. Production path now calls force_release_hold_budget_inbody
(same as #3254 inbody) so steal/GC see held==false before lexical end.

Contract (one row per AC):
  AC1  Production safepoint consume → force_release_hold_budget_inbody +
       forced_unlock_total bump
  AC2  Soft / !reject_enabled early-return unchanged (no consume)
  AC3  Outermost-only (this_fiber_outermost); nested never independently
  AC4  Tests extend hold_budget_synthetic_yield_injection; no invent/docs

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

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    fn = efm.find("aura_evaluator_try_hold_budget_fail_closed_at_safepoint() noexcept")
    if fn < 0:
        fails.append("AC1: fail-closed ABI missing")
        win = ""
    else:
        win = efm[fn : fn + 2800]

    must("Issue #3825", "AC1 cite", win)
    must("force_release_hold_budget_inbody", "AC1 force-release", win)
    must("g_mutation_hold_budget_forced_unlock_total", "AC1 unlock counter", win)
    must("g_mutation_hold_budget_forced_fail_closed_total", "AC1 fail-closed counter", win)
    # Regression: mark_failed-only must not be the sole action after consume.
    if "force_release_hold_budget_inbody" in win:
        consume = win.find("consume_hold_budget_cancel")
        after = win[consume:] if consume >= 0 else win
        fr = after.find("force_release_hold_budget_inbody")
        bare = after.find("g->mark_failed()")
        if fr < 0:
            fails.append("AC1: force_release after consume missing")
        elif bare >= 0 and bare < fr:
            fails.append("AC1: bare g->mark_failed() still precedes force_release")

    must("force_release_hold_budget_inbody", "AC1 emb impl", emb)
    must("force_release_hold_after_cancel_", "AC1 emb unlock helper", emb)

    must("mutation_hold_budget_reject_enabled()", "AC2 Soft gate", win)
    must("this_fiber_outermost()", "AC3 outermost-only", win)

    must("3825 AC1: held==false before Guard lexical end", "AC4 AC1 test", t)
    must("3825 AC1: forced_unlock_total bumps", "AC4 unlock test", t)
    must("3825 AC2: Soft does not force-fail", "AC4 Soft test", t)
    must("3825 AC3: Steal Ok — process held clear after unlock", "AC4 steal test", t)
    must("run_test_hold_budget_safepoint_force_release_3825", "AC4 runner", t)
    must("check_hold_budget_safepoint_force_release_3825", "AC4 build.py", build)

    if (ROOT / "tests" / "serve" / "test_issue_3825.cpp").is_file():
        fails.append("AC4: test_issue_3825.cpp present (forbidden invent)")
    if (ROOT / "tests" / "issues" / "test_issue_3825.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3825.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3825-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3825 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3825 safepoint fail-closed force-release — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

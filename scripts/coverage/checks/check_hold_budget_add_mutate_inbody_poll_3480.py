#!/usr/bin/env python3
"""Issue #3480: add_mutate polls inbody hold-budget after fn(a).

Force-release exists (#3222/#3254). Structural mutate entry did not
call it, so a non-coop body kept workspace_mtx_ until safepoint/dtor.
Poll existing inbody on the existing wrapper. Soft: no force-release.
Cross-fiber still only request_hold_budget_cancel.

Contract:
  AC1  production + cancel armed → force_release_hold_budget_inbody
  AC2  wrapper does not request_hold_budget_cancel (thief unlock)
  AC3  reject_enabled gate (Soft observe-only)
  AC4  reuse forced_unlock_total / forced_fail_closed_total
  AC5  extend test_hold_budget_synthetic_yield_injection
  AC6  linter AFTER #3222; no invent / docs/design

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/serve/test_hold_budget_synthetic_yield_injection.cpp")
    build = _read("build.py")

    start = mut.find("auto add_mutate = ")
    if start < 0:
        fails.append("AC6: add_mutate missing")
        win = ""
    else:
        win = mut[start : start + 14000]
        must("Issue #3480", "AC6 cite", win)
        must("auto result = fn(a);", "AC1 after fn(a)", win)
        fn = win.find("auto result = fn(a);")
        fr = win.find("force_release_hold_budget_inbody")
        if fn < 0 or fr < 0 or fr < fn:
            fails.append("AC1: force_release must follow fn(a)")
        must("aura_hold_budget_cancel_armed()", "AC1 cancel peek", win)
        must("aura_hold_budget_poll_inbody_window()", "AC1 poll", win)
        must("wrapper_guard->is_outermost()", "AC1 outermost", win)
        must("mutation_hold_budget_reject_enabled()", "AC3 Soft gate", win)
        must("g_mutation_hold_budget_forced_unlock_total", "AC4 unlock counter", win)
        must("g_mutation_hold_budget_forced_fail_closed_total", "AC4 fail-closed counter", win)
        must_not("aura_fiber_request_hold_budget_cancel", "AC2 no thief unlock", win)
        must("hold-budget-cancel", "AC1 structured error", win)

    must("force_release_hold_budget_inbody", "AC6 helper", emb)
    must("aura_fiber_request_hold_budget_cancel", "AC2 cross-fiber", emb)

    must("3480 AC1: workspace hold cleared after add_mutate", "AC5 AC1", t)
    must("3480 AC2: wrapper does not unlock from thief thread", "AC5 AC2", t)
    must("3480 AC3: wrapper does not force-release under Soft", "AC5 AC3", t)
    must("run_test_hold_budget_add_mutate_inbody_poll_3480", "AC5 runner", t)

    must_not("schema-3480", "AC4 no query key", mut)
    must_not("g_3480_", "AC4 no g_3480_*", mut)

    must("check_hold_budget_add_mutate_inbody_poll_3480", "AC6 build.py", build)
    prev = build.find("check_hold_budget_inbody_force_unlock_3222")
    ours = build.find("check_hold_budget_add_mutate_inbody_poll_3480")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3222")

    if (ROOT / "tests" / "serve" / "test_issue_3480.cpp").is_file():
        fails.append("AC6: test_issue_3480.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3480-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3480 hold_budget_add_mutate_inbody_poll:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3480 hold_budget_add_mutate_inbody_poll: wrapper polls; Soft no force-release")
    return 0


if __name__ == "__main__":
    sys.exit(main())

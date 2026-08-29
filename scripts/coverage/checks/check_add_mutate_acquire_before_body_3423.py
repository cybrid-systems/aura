#!/usr/bin/env python3
"""Issue #3423: add_mutate acquires Guard before fn(a).

#2986/#3197 post-check after fn(a) still returned naked-mutate after
a live write. #3423 moves mutate_dispatch_try_acquire textually
before fn(a) for !guard_exempt. GUARD_EXEMPT skips. Nested body
try_acquire stays the lockless nested path.

Contract:
  AC1 add_mutate lambda: mutate_dispatch_try_acquire before fn(a)
  AC2 acquire fail → guard-reject, no fn(a)
  AC3 GUARD_EXEMPT / metadata prims skip wrapper acquire
  AC4 #2986 wrap + #3197 token belt kept; #3074 sole-acquire
  AC5 tests in test_mutation_guard_try_acquire_unit; linter after
      #3352; no docs/design/3423-*; no test_issue_3423.cpp

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    disp = _read("src/compiler/mutate_dispatch.hh")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")

    must("kAddMutateAcquireBeforeBodyIssue = 3423", "AC1 stamp", disp)
    must("Issue #3423", "AC1 add_mutate cite", mut)

    lam = mut.find("auto add_mutate = [&](std::string name, auto fn, bool guard_exempt")
    if lam < 0:
        fails.append("AC1: add_mutate lambda missing")
        lam_win = ""
    else:
        # Window covering wrapper body through the post-check belt.
        lam_win = mut[lam : lam + 14000]

    must("mutate_dispatch_try_acquire", "AC1 wrapper acquire", lam_win)
    acq = lam_win.find("mutate_dispatch_try_acquire")
    fn_a = lam_win.find("auto result = fn(a)")
    if acq < 0 or fn_a < 0 or acq > fn_a:
        fails.append("AC1: mutate_dispatch_try_acquire must appear before auto result = fn(a)")
    must("if (!guard_exempt)", "AC1 exempt skip", lam_win)

    must("guard-reject", "AC2 reject kind", lam_win)
    # Acquire-fail return must precede the body call.
    rej = lam_win.find('mev("guard-reject"')
    if rej < 0 or fn_a < 0 or rej > fn_a:
        fails.append("AC2: guard-reject return must precede auto result = fn(a)")

    must("/*guard_exempt=*/true", "AC3 exempt flag", mut)
    must("mutate:set-agent-fingerprint", "AC3 fingerprint", mut)
    must("mutate:set-stale-ref-policy", "AC3 policy", mut)

    must("naked_mutate_attempt", "AC4 belt attempt", lam_win)
    must("mutate_guard_acquire_token", "AC4 belt token", lam_win)
    must("mutate_dispatch_try_acquire", "AC4 sole acquire", disp)
    must("mark_outermost_mutation_failed", "AC4 nested fail", bound)

    must("ac3423_1_acquire_before_fn", "AC5 test", t)
    must("check_add_mutate_acquire_before_body_3423", "AC5 build.py", build)
    prev = build.find("check_transform_engine_guard_3352")
    ours = build.find("check_add_mutate_acquire_before_body_3423")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3423 linter must run after #3352")
    if (ROOT / "tests" / "issues" / "test_issue_3423.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3423.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3423.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3423.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3423-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3423 add_mutate_acquire_before_body:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3423 add_mutate_acquire_before_body: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

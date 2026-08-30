#!/usr/bin/env python3
"""Issue #3450: add_mutate fences workspace_read_only_ before acquire.

replace-type / atomic-batch missed the per-body RO check. Wrapper now
loads workspace_read_only_ before mutate_dispatch_try_acquire so a
locked workspace never takes exclusive workspace_mtx_ for a no-op write.
GUARD_EXEMPT metadata/policy still runs. Per-body checks stay as belt.

Contract:
  AC1 Production locked + replace-type → read-only, log unchanged
  AC2 Production locked + atomic-batch → read-only, zero sub-op writes
  AC3 Unlocked replace-type / atomic-batch / set-body unchanged
  AC4 GUARD_EXEMPT fingerprint / policy still run on a locked workspace
  AC5 no docs/design/3450-*; no test_issue_3450.cpp; no new query key

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
    disp = _read("src/compiler/mutate_dispatch.hh")
    t = _read("tests/compiler/test_workspace_lock_unlock.cpp")
    g = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")

    lam = mut.find("auto add_mutate = [&](std::string name, auto fn, bool guard_exempt")
    win = mut[lam : lam + 14000] if lam >= 0 else ""
    must("kAddMutateReadOnlyFenceIssue", "AC1 stamp", disp)
    must("Issue #3450", "AC1 wrapper cite", win)
    must("workspace_read_only_", "AC1 RO load", win)
    must('mev("read-only"', "AC1 reuse read-only kind", win)
    ro = win.find("workspace_read_only_")
    acq = win.find("mutate_dispatch_try_acquire")
    fn_a = win.find("auto result = fn(a)")
    if ro < 0 or acq < 0 or ro > acq:
        fails.append("AC1: workspace_read_only_ must precede mutate_dispatch_try_acquire")
    if acq < 0 or fn_a < 0 or acq > fn_a:
        fails.append("AC1: acquire still before fn(a) (#3423)")
    must("3450 AC1: replace-type read-only", "AC1 test", t)

    must('add_mutate("mutate:atomic-batch"', "AC2 atomic-batch via add_mutate", mut)
    must("3450 AC2: atomic-batch read-only", "AC2 test", t)
    must("3450 AC2: zero sub-op writes", "AC2 no partial", t)

    must("if (ev.workspace_read_only_)", "AC3 per-body belt kept", mut)
    must("3450 AC3: unlocked replace-type not RO", "AC3 replace-type", t)
    must("3450 AC3: unlocked atomic-batch not RO", "AC3 batch", t)
    must("3450 AC3: unlocked set-body not RO", "AC3 set-body", t)

    must("!guard_exempt && ev.workspace_read_only_", "AC4 exempt skip", win)
    must("mutate:set-agent-fingerprint", "AC4 fingerprint", mut)
    must("/*guard_exempt=*/true", "AC4 exempt flag", mut)
    must("3450 AC4: fingerprint on locked ws", "AC4 test", t)

    must_not("schema-3450", "AC5 no new query key", mut)
    must("check_add_mutate_read_only_fence_3450", "AC5 build.py", build)
    must("check_add_mutate_acquire_before_body_3423", "AC5 #3423 retained", build)
    must("workspace_read_only_", "AC5 3423 unit cite", g)
    prev = build.find("check_add_mutate_acquire_before_body_3423")
    ours = build.find("check_add_mutate_read_only_fence_3450")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3450 linter must be wired after #3423")
    if (ROOT / "tests" / "compiler" / "test_issue_3450.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3450.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3450.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3450.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3450-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3450 add_mutate_read_only_fence:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3450 add_mutate_read_only_fence: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

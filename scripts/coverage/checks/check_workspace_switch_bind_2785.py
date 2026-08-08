#!/usr/bin/env python3
"""Issue #2785: workspace:switch single bind block (no duplicate assign).

Incomplete refactor left two assignment sequences; the first skipped
set_workspace_cow_epoch (#738). Consolidated to one block.

Contract (one row per AC):
  AC1 switch window has Issue #2785 + set_workspace_cow_epoch
  AC2 exactly one ev.workspace_flat_ = ws->flat and one wt->active()
  AC3 no nested \"ws = wt->active()\" re-fetch after first bind
  AC4 tests/compiler/test_workspace_switch.cpp + no test_issue_2785.cpp
  AC5 this linter wired; no docs/design/2785-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    test = _read("tests/compiler/test_workspace_switch.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = ws.find('["workspace:switch"]')
    if pos < 0:
        pos = ws.find("workspace:switch")
    if pos < 0:
        fails.append("AC1: workspace:switch not found")
        pos = 0
    end = ws.find('["workspace:current"]', pos)
    if end < 0:
        end = ws.find("workspace:current", pos)
    if end < 0:
        end = pos + 1500
    win = ws[pos:end]

    # AC1
    must("Issue #2785", "AC1", win)
    must("set_workspace_cow_epoch", "AC1", win)
    must("Issue #738", "AC1", win)

    # AC2 — single assign + single active()
    flat_n = win.count("ev.workspace_flat_ = ws->flat")
    if flat_n != 1:
        fails.append(f"AC2: expected 1 flat assign, found {flat_n}")
    active_n = win.count("wt->active()")
    if active_n != 1:
        fails.append(f"AC2: expected 1 wt->active(), found {active_n}")

    # AC3 — no second re-fetch after the initial `auto* ws = wt->active()`.
    # Count bare "ws = wt->active()" reassignments (exclude the declaration).
    residual = 0
    needle = "ws = wt->active()"
    p = 0
    while True:
        i = win.find(needle, p)
        if i < 0:
            break
        # Declaration form is "auto* ws = wt->active()" — skip those.
        prefix = win[max(0, i - 8) : i]
        if "auto*" not in prefix and "auto *" not in prefix:
            residual += 1
        p = i + len(needle)
    if residual != 0:
        fails.append(f"AC3: residual 'ws = wt->active()' re-fetch present ({residual})")

    # AC4
    must("ac2785", "AC4", test)
    must("2785", "AC4", test)
    must("set_workspace_cow_epoch", "AC4", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_switch.cpp").is_file():
        fails.append("AC4: missing test_workspace_switch.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2785.cpp").is_file():
        fails.append("AC4: test_issue_2785.cpp present (forbidden per #81967)")
    must("test_workspace_switch", "AC4", cmake)

    # AC5
    must("check_workspace_switch_bind_2785", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2785-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2785 workspace:switch consolidated bind — one active() + set_workspace_cow_epoch")
    return 0


if __name__ == "__main__":
    sys.exit(main())

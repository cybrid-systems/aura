#!/usr/bin/env python3
"""Issue #2786: workspace:lock only updates active quick flag.

workspace:lock previously set ev.workspace_read_only_ unconditionally,
while workspace:unlock only updated when idx == active_idx(). Locking a
non-active workspace poisoned mutate:* P6 permission checks for the
active workspace.

Contract (one row per AC):
  AC1 lock window cites #2786 and guards workspace_read_only_ with active_idx
  AC2 unlock window retains active_idx guard (symmetry)
  AC3 no unconditional workspace_read_only_ assign in lock without active_idx
  AC4 tests/compiler/test_workspace_lock_unlock.cpp + no test_issue_2786.cpp
  AC5 this linter wired; no docs/design/2786-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _window(src: str, start_needles: list[str], end_needles: list[str], fallback: int) -> str:
    pos = -1
    for n in start_needles:
        pos = src.find(n)
        if pos >= 0:
            break
    if pos < 0:
        return ""
    end = -1
    for n in end_needles:
        end = src.find(n, pos + 1)
        if end >= 0:
            break
    if end < 0:
        end = pos + fallback
    return src[pos:end]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    test = _read("tests/compiler/test_workspace_lock_unlock.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    lwin = _window(
        ws,
        ['["workspace:lock"]', "workspace:lock"],
        ['["workspace:unlock"]', "workspace:unlock"],
        900,
    )
    uwin = _window(
        ws,
        ['["workspace:unlock"]', "workspace:unlock"],
        ["workspace:can-write?", "can-write?"],
        700,
    )

    if not lwin:
        fails.append("AC1: workspace:lock not found")
    if not uwin:
        fails.append("AC2: workspace:unlock not found")

    # AC1
    must("Issue #2786", "AC1", lwin)
    must("active_idx()", "AC1", lwin)
    must("workspace_read_only_", "AC1", lwin)
    if lwin:
        apos = lwin.find("active_idx()")
        rpos = lwin.find("workspace_read_only_")
        if not (apos >= 0 and rpos >= 0 and apos < rpos):
            fails.append("AC1: active_idx() must precede workspace_read_only_ assign")

    # AC2 — unlock symmetry
    must("active_idx()", "AC2", uwin)
    must("workspace_read_only_", "AC2", uwin)

    # AC3 — forbid bare unconditional assign (old bug shape).
    # After set_read_only, must have if (idx == wt->active_idx()) before assign.
    if lwin and not re.search(
        r"set_read_only\s*\([^)]*\)\s*;\s*"
        r"(?://[^\n]*\n\s*)*"
        r"(?:/\*.*?\*/\s*)*"
        r"if\s*\(\s*idx\s*==\s*wt\s*->\s*active_idx\s*\(\s*\)\s*\)\s*"
        r"ev\.workspace_read_only_\s*=",
        lwin,
        re.DOTALL,
    ):
        fails.append("AC3: lock must guard workspace_read_only_ with if (idx == wt->active_idx()) after set_read_only")

    # AC4
    must("ac2786", "AC4", test)
    must("2786", "AC4", test)
    must("active_idx", "AC4", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_lock_unlock.cpp").is_file():
        fails.append("AC4: missing test_workspace_lock_unlock.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2786.cpp").is_file():
        fails.append("AC4: test_issue_2786.cpp present (forbidden per #81967)")
    must("test_workspace_lock_unlock", "AC4", cmake)

    # AC5
    must("check_workspace_lock_active_flag_2786", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2786-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2786 workspace:lock active-only quick flag — symmetric with unlock active_idx guard")
    return 0


if __name__ == "__main__":
    sys.exit(main())

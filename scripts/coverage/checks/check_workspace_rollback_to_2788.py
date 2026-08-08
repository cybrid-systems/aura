#!/usr/bin/env python3
"""Issue #2788: workspace:rollback-to consistent name→id resolve.

Name lookup previously scanned snapshot_names_ then called restore
without validating snapshot_sources_ size under the same view.
Silent #f hid not-found vs concurrent-delete / pair drift.

Contract (one row per AC):
  AC1 cites #2788; WorkspaceUniqueIfNeeded; make_merr not-found/concurrent-delete
  AC2 name hit checks i >= sources_n; restore after lock scope
  AC3 tests/compiler/test_workspace_rollback_to.cpp + no test_issue_2788.cpp
  AC4 this linter wired; no docs/design/2788-*

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    test = _read("tests/compiler/test_workspace_rollback_to.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = ws.find("workspace:rollback-to")
    if pos < 0:
        fails.append("AC1: workspace:rollback-to not found")
        win = ""
    else:
        end = ws.find("workspace:rollback-latest", pos)
        if end < 0:
            end = pos + 3500
        win = ws[pos:end]

    # AC1
    must("Issue #2788", "AC1", win)
    must("WorkspaceUniqueIfNeeded", "AC1", win)
    must("make_merr", "AC1", win)
    must("not-found", "AC1", win)
    must("concurrent-delete", "AC1", win)

    # AC2 — paired size check on name hit; restore after lock
    must("sources_n", "AC2", win)
    must("i >= sources_n", "AC2", win)
    if "snapshot_names_" in win and "snapshot_sources_" not in win:
        fails.append("AC2: reads names_ without observing sources_")
    # Forbid old silent make_bool(false) as sole failure path for name miss.
    # Allow make_bool only if make_merr also present (success still uses restore #t).
    if re.search(r"return make_bool\s*\(\s*false\s*\)", win) and "make_merr" not in win:
        fails.append("AC2: residual silent make_bool(false) without make_merr")

    # AC3
    must("ac2788", "AC3", test)
    must("2788", "AC3", test)
    must("concurrent-delete", "AC3", test)
    must("not-found", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_rollback_to.cpp").is_file():
        fails.append("AC3: missing test_workspace_rollback_to.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2788.cpp").is_file():
        fails.append("AC3: test_issue_2788.cpp present (forbidden per #81967)")
    must("test_workspace_rollback_to", "AC3", cmake)

    # AC4
    must("check_workspace_rollback_to_2788", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2788-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2788 workspace:rollback-to typed resolve — locked pair view + not-found/concurrent-delete")
    return 0


if __name__ == "__main__":
    sys.exit(main())

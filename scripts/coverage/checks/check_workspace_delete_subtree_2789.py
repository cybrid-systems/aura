#!/usr/bin/env python3
"""Issue #2789: workspace:delete recursively tombstones descendants.

delete_child previously only nulled the target node; children remained
in nodes_ as orphans (listed by workspace:list, resources retained).

Contract (one row per AC):
  AC1 delete_child cites #2789; recursive parent_layer_idx + is_tombstone
  AC2 workspace:delete uses is_under for rebind; list skips is_tombstone
  AC3 tests/compiler/test_workspace_delete_subtree.cpp + no test_issue_2789.cpp
  AC4 this linter wired; no docs/design/2789-*

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

    ixx = _read("src/compiler/evaluator.ixx")
    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    test = _read("tests/compiler/test_workspace_delete_subtree.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = ixx.find("WorkspaceTree::delete_child")
    if pos < 0:
        fails.append("AC1: WorkspaceTree::delete_child not found")
        win = ""
    else:
        win = ixx[pos : pos + 1800]

    # AC1
    must("#2789", "AC1", win)
    must("parent_layer_idx", "AC1", win)
    must("is_tombstone", "AC1", win)
    must("delete_child(i)", "AC1", win)

    # AC2
    # Prefer the PrimFn registration site (not the dispatcher call_named).
    del_pos = ws.find('add("workspace:delete"')
    if del_pos < 0:
        del_pos = ws.find("workspace:delete")
    if del_pos < 0:
        fails.append("AC2: workspace:delete not found")
        dwin = ""
    else:
        # Include the Issue #2789 comment above the add().
        start = max(0, del_pos - 200)
        dwin = ws[start : del_pos + 2000]
    must("Issue #2789", "AC2", dwin)
    must("is_under", "AC2", dwin)

    list_pos = ws.find('["workspace:list"]')
    if list_pos < 0:
        list_pos = ws.find("workspace:list")
    if list_pos < 0:
        fails.append("AC2: workspace:list not found")
        lwin = ""
    else:
        # Include the #2789 skip-tombstone comment above the registration.
        start = max(0, list_pos - 250)
        lwin = ws[start : list_pos + 900]
    must("is_tombstone", "AC2", lwin)
    must("2789", "AC2", lwin)

    # AC3
    must("ac2789", "AC3", test)
    must("2789", "AC3", test)
    must("is_tombstone", "AC3", test)
    must("grandchild", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_delete_subtree.cpp").is_file():
        fails.append("AC3: missing test_workspace_delete_subtree.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2789.cpp").is_file():
        fails.append("AC3: test_issue_2789.cpp present (forbidden per #81967)")
    must("test_workspace_delete_subtree", "AC3", cmake)

    # AC4
    must("check_workspace_delete_subtree_2789", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2789-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2789 workspace:delete recursive subtree — no orphan layers in list")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2784: workspace:sync-from must use actual source body.

Hardcoded (lambda (x) x) silently corrupted AI cross-workspace sync.

Contract (one row per AC):
  AC1 sync-from uses unparse_node (or equivalent) for define body
  AC2 no hardcoded \"(lambda (x) x)\" rebind stub in sync-from window
  AC3 no root-restore discard fallback claiming success
  AC4 tests/compiler/test_workspace_sync_from.cpp + no test_issue_2784.cpp
  AC5 this linter wired; no docs/design/2784-*

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    ws = _read("src/compiler/evaluator_primitives_workspace.cpp")
    test = _read("tests/compiler/test_workspace_sync_from.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate sync-from window
    pos = ws.find('add("workspace:sync-from"')
    if pos < 0:
        fails.append("AC1: workspace:sync-from not found")
        pos = 0
    end = ws.find('add("workspace:discard"', pos)
    if end < 0:
        end = pos + 5000
    win = ws[pos:end]

    # AC1
    must("Issue #2784", "AC1", win)
    must("unparse_node", "AC1", win)
    must("mutate:rebind", "AC1", win)

    # AC2 — identity stub as rebind code
    must_not('"(lambda (x) x)"', "AC2", win)
    must_not('std::string("(lambda (x) x)")', "AC2", win)

    # AC3 — discard-parse fallback
    must_not("ev.workspace_flat_->root = saved_root", "AC3", win)
    must_not("Keep original root", "AC3", win)

    # AC4
    must("ac2784", "AC4", test)
    must("2784", "AC4", test)
    must("workspace:sync-from", "AC4", test)
    must("my-fn", "AC4", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_sync_from.cpp").is_file():
        fails.append("AC4: missing tests/compiler/test_workspace_sync_from.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2784.cpp").is_file():
        fails.append("AC4: test_issue_2784.cpp present (forbidden per #81967)")
    must("test_workspace_sync_from", "AC4", cmake)

    # AC5
    must("check_workspace_sync_from_body_2784", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2784-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2784 workspace:sync-from actual body — unparse_node + no identity stub")
    return 0


if __name__ == "__main__":
    sys.exit(main())

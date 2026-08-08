#!/usr/bin/env python3
"""Issue #2787: workspace:rollback-latest single reverse walk.

Previously the prim reverse-walked the log then re-searched by
mutation_id (O(N) per rollback → O(N²) sequential). Concurrent
append could also match the wrong record by ID.

Contract (one row per AC):
  AC1 cites #2787; uses try_rollback_record; index reverse walk
  AC2 no second-walk by mutation_id / no rollback(mid) re-search
  AC3 tests/compiler/test_workspace_rollback_latest.cpp + no test_issue_2787.cpp
  AC4 this linter wired; no docs/design/2787-*

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
    test = _read("tests/compiler/test_workspace_rollback_latest.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = ws.find("workspace:rollback-latest")
    if pos < 0:
        fails.append("AC1: workspace:rollback-latest not found")
        win = ""
    else:
        end = ws.find("workspace:mutation-count", pos)
        if end < 0:
            end = pos + 2500
        win = ws[pos:end]

    # AC1
    must("Issue #2787", "AC1", win)
    must("try_rollback_record", "AC1", win)
    if not re.search(r"for\s*\(\s*std::size_t\s+ri\b", win):
        fails.append("AC1: missing index reverse walk `for (std::size_t ri`")

    # AC2 — forbid old double-walk shapes
    if "r.mutation_id == it->mutation_id" in win:
        fails.append("AC2: residual second-walk `r.mutation_id == it->mutation_id`")
    # Old field path: rollback(mid) re-searches the log by id.
    if re.search(r"->rollback\s*\(\s*mid\s*\)", win) or re.search(r"\.rollback\s*\(\s*mid\s*\)", win):
        fails.append("AC2: residual rollback(mid) re-search")
    # Forbid nested all_mutations loop used only for status flip by id.
    nested = re.search(
        r"for\s*\(\s*auto\s*&\s*r\s*:\s*.*all_mutations\s*\(\s*\)\s*\)\s*\{[^}]*mutation_id",
        win,
        re.DOTALL,
    )
    if nested:
        fails.append("AC2: residual nested all_mutations() ID walk")

    # AC3
    must("ac2787", "AC3", test)
    must("2787", "AC3", test)
    must("try_rollback_record", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_workspace_rollback_latest.cpp").is_file():
        fails.append("AC3: missing test_workspace_rollback_latest.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2787.cpp").is_file():
        fails.append("AC3: test_issue_2787.cpp present (forbidden per #81967)")
    must("test_workspace_rollback_latest", "AC3", cmake)

    # AC4
    must("check_workspace_rollback_latest_2787", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2787-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2787 workspace:rollback-latest single-walk — try_rollback_record, no mutation_id re-search")
    return 0


if __name__ == "__main__":
    sys.exit(main())

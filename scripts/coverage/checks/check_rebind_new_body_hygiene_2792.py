#!/usr/bin/env python3
"""Issue #2792: mutate:rebind hygiene-checks new_value subtree.

Pre-parse hygiene only probed old_define. MacroIntroduced bodies
could be rebound onto normal Defines, defeating #373.

Contract (one row per AC):
  AC1 rebind cites #2792; walk_subtree(new_value) + is_macro_introduced
  AC2 free orphans on new-body hygiene reject
  AC3 tests/compiler/test_rebind_new_body_hygiene.cpp + no test_issue_2792.cpp
  AC4 this linter wired; no docs/design/2792-*

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_rebind_new_body_hygiene.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add_mutate("mutate:rebind"')
    if pos < 0:
        pos = mut.find("mutate:rebind")
    if pos < 0:
        fails.append("AC1: mutate:rebind not found")
        win = ""
    else:
        end = mut.find('add_mutate("mutate:set-body"', pos)
        if end < 0:
            end = pos + 8000
        win = mut[pos:end]

    # AC1
    must("Issue #2792", "AC1", win)
    must("walk_subtree(new_value", "AC1", win)
    must("is_macro_introduced", "AC1", win)

    # AC2
    if "free_rebind_parse_orphans" not in win and "free_orphan_nodes_from" not in win:
        fails.append("AC2: missing free orphans on hygiene reject path")
    # Reject path should call hygiene_protected_error for the hit.
    must("hygiene_protected_error", "AC2", win)

    # AC3
    must("ac2792", "AC3", test)
    must("2792", "AC3", test)
    must("walk_subtree", "AC3", test)
    must("hygiene-protected", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_rebind_new_body_hygiene.cpp").is_file():
        fails.append("AC3: missing test_rebind_new_body_hygiene.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2792.cpp").is_file():
        fails.append("AC3: test_issue_2792.cpp present (forbidden per #81967)")
    must("test_rebind_new_body_hygiene", "AC3", cmake)

    # AC4
    must("check_rebind_new_body_hygiene_2792", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2792-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2792 rebind new-body hygiene — walk_subtree(new_value) + MacroIntroduced reject")
    return 0


if __name__ == "__main__":
    sys.exit(main())

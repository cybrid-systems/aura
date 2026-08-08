#!/usr/bin/env python3
"""Issue #2801: move-node MacroIntroduced hygiene (#142 parity with replace-subtree).

Public + lockless reject is_macro_introduced(node) before detach/insert;
move_node_hygiene_reject_total metric on FlatAST.

Contract (one row per AC):
  AC1 public + lockless cite #2801; is_macro_introduced + note_move_node_hygiene_reject
  AC2 move_node_hygiene_reject metric on FlatAST
  AC3 tests/compiler/test_move_node_hygiene.cpp + no test_issue_2801.cpp
  AC4 this linter wired; no docs/design/2801-*

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
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ast = _read("src/core/ast.ixx")
    test = _read("tests/compiler/test_move_node_hygiene.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    ppos = mut.find('add_mutate("mutate:move-node"')
    if ppos < 0:
        ppos = mut.find("mutate:move-node")
    pwin = mut[ppos : ppos + 4000] if ppos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_move_node")
    lwin = flat[lpos : lpos + 2500] if lpos >= 0 else ""

    # AC1
    must("Issue #2801", "AC1", pwin)
    must("is_macro_introduced", "AC1", pwin)
    must("note_move_node_hygiene_reject", "AC1", pwin)
    must("hygiene", "AC1", pwin)
    must("Issue #2801", "AC1", lwin)
    must("is_macro_introduced", "AC1", lwin)
    must("note_move_node_hygiene_reject", "AC1", lwin)
    must("cannot move macro-introduced", "AC1", lwin)

    # AC2
    must("move_node_hygiene_reject", "AC2", ast)
    must("Issue #2801", "AC2", ast)
    must("note_move_node_hygiene_reject", "AC2", ast)

    # AC3
    must("ac2801", "AC3", test)
    must("2801", "AC3", test)
    must("is_macro_introduced", "AC3", test)
    must("move_node_hygiene_reject", "AC3", test)
    must("move-node", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_move_node_hygiene.cpp").is_file():
        fails.append("AC3: missing test_move_node_hygiene.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2801.cpp").is_file():
        fails.append("AC3: test_issue_2801.cpp present (forbidden per #81967)")
    must("test_move_node_hygiene", "AC3", cmake)

    # AC4
    must("check_move_node_hygiene_2801", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2801-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2801 move-node MacroIntroduced hygiene — reject + move_node_hygiene_reject_total")
    return 0


if __name__ == "__main__":
    sys.exit(main())

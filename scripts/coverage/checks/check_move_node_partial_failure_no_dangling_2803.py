#!/usr/bin/env python3
"""Issue #2803: move-node reattaches on insert failure (no NULL_NODE dangling hole).

try_move_child detaches then inserts; on failure reattaches and bumps
move_node_partial_failure_dangling_prevented_total.

Contract (one row per AC):
  AC1 public + lockless cite #2803; try_move_child
  AC2 FlatAST try_move_child + metric + inject hook
  AC3 tests/compiler/test_move_node_partial_failure_no_dangling.cpp
  AC4 this linter wired; no docs/design/2803-*; no test_issue_2803.cpp

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
    test = _read("tests/compiler/test_move_node_partial_failure_no_dangling.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    ppos = mut.find('add_mutate("mutate:move-node"')
    if ppos < 0:
        ppos = mut.find("mutate:move-node")
    pwin = mut[ppos : ppos + 4500] if ppos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_move_node")
    # Body through try_move_child (~3.3KB+)
    lwin = flat[lpos : lpos + 4500] if lpos >= 0 else ""

    # AC1
    must("Issue #2803", "AC1", pwin)
    must("try_move_child", "AC1", pwin)
    must("Issue #2803", "AC1", lwin)
    must("try_move_child", "AC1", lwin)

    # AC2
    must("try_move_child", "AC2", ast)
    must("move_node_partial_failure_dangling_prevented", "AC2", ast)
    must("note_move_node_partial_failure_dangling_prevented", "AC2", ast)
    must("set_test_inject_insert_child_fail_once", "AC2", ast)
    must("Issue #2803", "AC2", ast)

    # AC3
    must("ac2803", "AC3", test)
    must("2803", "AC3", test)
    must("try_move_child", "AC3", test)
    must("move_node_partial_failure_dangling_prevented", "AC3", test)
    must("set_test_inject_insert_child_fail_once", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_move_node_partial_failure_no_dangling.cpp").is_file():
        fails.append("AC3: missing test_move_node_partial_failure_no_dangling.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2803.cpp").is_file():
        fails.append("AC3: test_issue_2803.cpp present (forbidden per #81967)")
    must("test_move_node_partial_failure_no_dangling", "AC3", cmake)

    # AC4
    must("check_move_node_partial_failure_no_dangling_2803", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2803-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2803 move-node partial-failure no dangling — try_move_child reattach + metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())

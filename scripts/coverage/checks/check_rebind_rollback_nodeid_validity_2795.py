#!/usr/bin/env python3
"""Issue #2795: rebind captures old body NodeId after parse; rollback validates.

Contract (one row per AC):
  AC1 rebind + batch-rebind cite #2795; live/free check before log
  AC2 try_rollback_rebind_op validates old_child + metric
  AC3 tests/compiler/test_rebind_rollback_nodeid_validity.cpp + no test_issue_2795.cpp
  AC4 this linter wired; no docs/design/2795-*

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
    test = _read("tests/compiler/test_rebind_rollback_nodeid_validity.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add_mutate("mutate:rebind"')
    if pos < 0:
        pos = mut.find("mutate:rebind")
    win = mut[pos : pos + 20000] if pos >= 0 else ""

    # AC1
    must("Issue #2795", "AC1", win)
    must("old_value_node", "AC1", win)
    must("is_live_node", "AC1", win)
    must("note_rebind_rollback_stale_nodeid_prevented", "AC1", win)
    parse_pos = win.find("parse_to_flat")
    capture_pos = win.find("old_value_node")
    if parse_pos < 0 or capture_pos < 0 or capture_pos <= parse_pos:
        fails.append("AC1: old_value_node must appear after parse_to_flat in rebind body")
    resolve_pos = win.find("resolve_define_after_parse")
    if resolve_pos < 0 or resolve_pos >= capture_pos:
        fails.append("AC1: resolve_define_after_parse must precede old_value capture")
    must("Issue #2795", "AC1", flat)

    # AC2
    must("try_rollback_rebind_op", "AC2", ast)
    must("Issue #2795", "AC2", ast)
    must("rebind_rollback_stale_nodeid_prevented", "AC2", ast)
    must("is_free_slot", "AC2", ast)

    # AC3
    must("ac2795", "AC3", test)
    must("2795", "AC3", test)
    must("old_value_node", "AC3", test)
    must("try_rollback", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_rebind_rollback_nodeid_validity.cpp").is_file():
        fails.append("AC3: missing test_rebind_rollback_nodeid_validity.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2795.cpp").is_file():
        fails.append("AC3: test_issue_2795.cpp present (forbidden per #81967)")
    must("test_rebind_rollback_nodeid_validity", "AC3", cmake)

    # AC4
    must("check_rebind_rollback_nodeid_validity_2795", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2795-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2795 rebind rollback NodeId validity — post-parse capture + free-slot reject")
    return 0


if __name__ == "__main__":
    sys.exit(main())

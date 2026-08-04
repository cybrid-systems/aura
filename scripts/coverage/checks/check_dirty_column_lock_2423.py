#!/usr/bin/env python3
"""Issue #2423: dirty_ column shared/exclusive lock for short-circuit APIs.

Contract:
  AC1 dirty_column_mtx_ + shared_lock on dirty_nodes_in_range / is_subtree_dirty_node
  AC2 mark_dirty exclusive lock for dirty_ RMW/resize
  AC3 misleading "const and thread-safe" claim removed; #2423 documented
  AC4 gate + test wired

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
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_dirty_column_lock.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2423", "AC1", ast)
    must("dirty_column_mtx_", "AC1", ast)
    must("dirty_nodes_in_range", "AC1", ast)
    must("is_subtree_dirty_node", "AC1", ast)
    # shared lock appears near the short-circuit readers
    must("std::shared_lock<std::shared_mutex> rlock(dirty_column_mtx_.mutable_get())", "AC1", ast)
    must("2423 AC1", "AC1", test)

    must("std::unique_lock<std::shared_mutex> wlock(dirty_column_mtx_.mutable_get())", "AC2", ast)
    must("mark_dirty", "AC2", ast)
    must("2423 AC2", "AC2", test)

    must_not("Both are const and thread-safe.", "AC3", ast)
    must("dirty_column_mtx_", "AC3", ast)
    must("2423 AC3", "AC3", test)

    must("2423 AC4", "AC4", test)
    must("check_dirty_column_lock_2423", "gate", build)
    must("cmd_dirty_column_lock_coverage", "gate", build)
    must("test_dirty_column_lock", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: dirty_column lock #2423 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

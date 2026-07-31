#!/usr/bin/env python3
"""Issue #2419: tag_arity_index_ protected by dedicated shared_mutex.

Contract:
  AC1 find_by_tag_arity uses shared_lock on tag_arity_index_mtx_
  AC2 rebuild/ensure use exclusive unique_lock
  AC3 mark_dirty live-patch under exclusive lock
  AC4 mutex declaration + gate wired

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_tag_arity_index_lock_2419.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2419", "AC1", ast)
    must("tag_arity_index_mtx_", "AC1", ast)
    must("find_by_tag_arity", "AC1", ast)
    must("shared_lock<std::shared_mutex> rlock(tag_arity_index_mtx_.mutable_get())", "AC1", ast)
    must("2419 AC1", "AC1", test)

    must("unique_lock<std::shared_mutex> wlock(tag_arity_index_mtx_.mutable_get())", "AC2", ast)
    must("rebuild_tag_arity_index_unlocked", "AC2", ast)
    must("ensure_tag_arity_index_unlocked", "AC2", ast)
    must("2419 AC2", "AC2", test)

    must("2419 AC3", "AC3", test)
    must("2419 AC4", "AC4", test)

    must("check_tag_arity_index_lock_2419", "gate", build)
    must("cmd_tag_arity_index_lock_coverage", "gate", build)
    must("test_tag_arity_index_lock_2419", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: tag_arity_index lock #2419 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

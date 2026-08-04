#!/usr/bin/env python3
"""Issue #2416: incoming_parent_index_dirty_ is std::atomic<bool>.

Contract:
  AC1 atomic declaration (not plain bool)
  AC2 mark/rebuild/ensure use store/load with memory orders
  AC3 copy/move use load/store (not assignment of bool)
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
    test = _read("tests/core/test_incoming_parent_dirty_atomic_2416.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2416", "AC1", ast)
    must("mutable std::atomic<bool> incoming_parent_index_dirty_{true}", "AC1", ast)
    must_not("mutable bool incoming_parent_index_dirty_ = true", "AC1", ast)
    must("2416 AC1", "AC1", test)

    must("incoming_parent_index_dirty_.store(true, std::memory_order_release)", "AC2", ast)
    must("incoming_parent_index_dirty_.store(false, std::memory_order_release)", "AC2", ast)
    must("incoming_parent_index_dirty_.load(std::memory_order_acquire)", "AC2", ast)
    must("2416 AC2", "AC2", test)

    must("incoming_parent_index_dirty_.load(std::memory_order_relaxed)", "AC3", ast)
    must("2416 AC3", "AC3", test)
    # No bare assignment of the flag as a bool
    must_not("incoming_parent_index_dirty_ = true", "AC3", ast)
    must_not("incoming_parent_index_dirty_ = false", "AC3", ast)
    must_not("incoming_parent_index_dirty_ = other.incoming_parent_index_dirty_;", "AC3", ast)

    must("2416 AC4", "AC4", test)
    must("check_incoming_parent_dirty_atomic_2416", "gate", build)
    must("cmd_incoming_parent_dirty_atomic_coverage", "gate", build)
    must("test_incoming_parent_dirty_atomic_2416", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: incoming_parent_dirty atomic #2416 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

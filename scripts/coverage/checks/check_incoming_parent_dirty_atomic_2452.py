#!/usr/bin/env python3
"""Issue #2452: incoming_parent_index_dirty_ atomic (stale-free parent edges).

Contract (already fixed by #2416; this gate cites #2452 ACs):
  AC1 atomic declaration + release mark / acquire ensure
  AC2 concurrent mark + dirty load covered in test_ast_concurrency
  AC3 collect rebuild after mark; gate wiring

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
    conc = _read("tests/core/test_ast_concurrency.cpp")
    test2416 = _read("tests/core/test_incoming_parent_dirty_atomic.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2452", "AC1", ast)
    must("mutable std::atomic<bool> incoming_parent_index_dirty_{true}", "AC1", ast)
    must("incoming_parent_index_dirty_.store(true, std::memory_order_release)", "AC1", ast)
    must("incoming_parent_index_dirty_.load(std::memory_order_acquire)", "AC1", ast)
    must_not("mutable bool incoming_parent_index_dirty_ = true", "AC1", ast)

    must("Issue #2452", "AC2", conc)
    must("mark_incoming_parent_index_dirty", "AC2", conc)
    must("#2452: readers observed dirty true", "AC2", conc)
    must("2416 AC2", "AC2", test2416)

    must("collect_incoming_parent_edges", "AC3", conc)
    must("#2452: collect sees parent edge after mark", "AC3", conc)
    must("check_incoming_parent_dirty_atomic_2452", "gate", build)
    must("cmd_incoming_parent_dirty_atomic_2452_coverage", "gate", build)
    must("test_ast_concurrency", "gate", cmake)
    must("test_incoming_parent_dirty_atomic", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: incoming_parent_dirty atomic #2452 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

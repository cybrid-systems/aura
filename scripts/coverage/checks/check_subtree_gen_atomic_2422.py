#!/usr/bin/env python3
"""Issue #2422: subtree_gen_ cells are atomic (uint32 + atomic_ref).

Contract:
  AC1 uint32 cells + atomic_ref load/store (no plain uint16 element access)
  AC2 concurrent bump + is_valid_subtree covered by gate test
  AC3 #392 semantics preserved (helpers + test)
  AC4 is_always_lock_free static_assert + gate wiring

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
    test = _read("tests/core/test_subtree_gen_atomic_2422.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2422", "AC1", ast)
    must("std::pmr::vector<std::uint32_t> subtree_gen_", "AC1", ast)
    must("std::atomic_ref<std::uint32_t>", "AC1", ast)
    must("load_subtree_gen", "AC1", ast)
    must_not("std::pmr::vector<std::uint16_t> subtree_gen_", "AC1", ast)
    must("2422 AC1", "AC1", test)

    must("subtree_gen_mtx_", "AC2", ast)
    must("2422 AC2", "AC2", test)
    must("concurrent bump", "AC2", test)

    must("is_valid_subtree", "AC3", ast)
    must("bump_generation_subtree", "AC3", ast)
    must("2422 AC3", "AC3", test)

    must("is_always_lock_free", "AC4", ast)
    must("std::atomic<std::uint32_t>::is_always_lock_free", "AC4", ast)
    must("2422 AC4", "AC4", test)

    must("check_subtree_gen_atomic_2422", "gate", build)
    must("cmd_subtree_gen_atomic_coverage", "gate", build)
    must("test_subtree_gen_atomic_2422", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: subtree_gen atomic #2422 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

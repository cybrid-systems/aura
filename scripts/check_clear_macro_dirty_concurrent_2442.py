#!/usr/bin/env python3
"""Issue #2442: clear_macro_dirty_all concurrent-safe vs macro_dirty readers.

Contract:
  AC1 exclusive dirty_column_mtx_ + atomic store in clear_macro_dirty_all
  AC2 concurrent clear+read covered by gate test
  AC3 clear zeros all bits (test + semantics)

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
    test = _read("tests/core/test_clear_macro_dirty_concurrent_2442.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2442", "AC1", ast)
    must("clear_macro_dirty_all", "AC1", ast)
    must("dirty_column_mtx_.mutable_get()", "AC1", ast)
    must("std::atomic_ref<std::uint8_t>(b).store(0", "AC1", ast)
    must("unique_lock", "AC1", ast)
    must("2442 AC1", "AC1", test)

    must("macro_dirty(NodeId id)", "AC2", ast)
    must("shared_lock", "AC2", ast)
    must("2442 AC2", "AC2", test)

    must("2442 AC3", "AC3", test)
    must("macro_dirty_count", "AC3", ast)

    must("check_clear_macro_dirty_concurrent_2442", "gate", build)
    must("cmd_clear_macro_dirty_concurrent_coverage", "gate", build)
    must("test_clear_macro_dirty_concurrent_2442", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: clear_macro_dirty concurrent #2442 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2441: apply_macro_dirty_bits metric double-count fix.

Contract:
  AC1 exclusive dirty_column_mtx_ + atomic fetch_or for newly_set
  AC2 concurrent multi-id write+read covered by gate test
  AC3 bit semantics preserved (macro_dirty accessor + test)
  AC4 clear/count + gate wiring

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
    test = _read("tests/core/test_macro_dirty_bits_lock_2441.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2441", "AC1", ast)
    must("apply_macro_dirty_bits", "AC1", ast)
    must("dirty_column_mtx_.mutable_get()", "AC1", ast)
    must("fetch_or(reasons", "AC1", ast)
    must("newly_set = static_cast<std::uint8_t>(reasons & ~prev)", "AC1", ast)
    must("macro_dirty_", "AC1", ast)
    must("2441 AC1", "AC1", test)

    must("2441 AC2", "AC2", test)
    must("macro_dirty(NodeId id)", "AC2", ast)

    must("2441 AC3", "AC3", test)
    must("kMacroExpansion", "AC3", ast)

    must("clear_macro_dirty_all", "AC4", ast)
    must("macro_dirty_count", "AC4", ast)
    must("2441 AC4", "AC4", test)
    must("check_macro_dirty_bits_lock_2441", "gate", build)
    must("cmd_macro_dirty_bits_lock_coverage", "gate", build)
    must("test_macro_dirty_bits_lock_2441", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: macro dirty bits lock #2441 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

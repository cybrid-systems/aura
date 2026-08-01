#!/usr/bin/env python3
"""Issue #2444: region_by_sym_dense_ race-free vs concurrent set_function_region.

Contract:
  AC1 exclusive region_table_mtx_ + atomic_ref on set_function_region dense path
  AC2 shared_lock + atomic load on get_function_region_for_sym
  AC3 concurrent test in test_ast_concurrency.cpp (+ dense path retained)

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
    test = _read("tests/core/test_ast_concurrency.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2444", "AC1", ast)
    must("region_by_sym_dense_", "AC1", ast)
    must("set_function_region", "AC1", ast)
    must("region_table_mtx_", "AC1", ast)
    must("std::atomic_ref<std::uint8_t>(region_by_sym_dense_", "AC1", ast)
    must("unique_lock", "AC1", ast)

    must("get_function_region_for_sym", "AC2", ast)
    must("shared_lock", "AC2", ast)
    must("memory_order_acquire", "AC2", ast)

    must("Issue #2444", "AC3", test)
    must("set_function_region", "AC3", test)
    must("get_function_region_for_sym", "AC3", test)
    must("#2444: writers progressed", "AC3", test)

    must("check_region_sym_dense_race_2444", "gate", build)
    must("cmd_region_sym_dense_race_coverage", "gate", build)
    # Existing concurrency harness target (extended for #2444).
    must("test_ast_concurrency", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: region_by_sym_dense race #2444 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

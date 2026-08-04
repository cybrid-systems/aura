#!/usr/bin/env python3
"""Issue #2408: string_bytes_total single shared_lock + resolve_unlocked.

Contract:
  AC1 concurrent intern + string_bytes_total (test + source-cite)
  AC2 sequential correctness
  AC3 one shared_lock per call (resolve_unlocked under held lock)
  AC4 buf_fragmentation / observability still use string_bytes_total

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

    def must_not_in_fn(fn_name: str, bad: str, label: str, hay: str) -> None:
        # Find function body and ensure bad pattern not used for resolve-in-loop.
        idx = hay.find(fn_name)
        if idx < 0:
            fails.append(f"{label}: function {fn_name!r} not found")
            return
        # crude: next 800 chars
        body = hay[idx : idx + 800]
        if bad in body:
            fails.append(f"{label}: {fn_name} still contains {bad!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_stringpool_bytes_total_lock.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2408", "AC1", ast)
    must("resolve_unlocked", "AC1", ast)
    must("2408 AC1", "AC1", test)
    must("string_bytes_total", "AC1", test)

    must("2408 AC2", "AC2", test)

    must("shared_lock", "AC3", ast)
    must("g_stringpool_intern_concurrent_readers_total", "AC3", ast)
    must_not_in_fn("string_bytes_total()", "resolve(hash_tbl_", "AC3", ast)
    must("2408 AC3", "AC3", test)

    must("buf_fragmentation", "AC4", ast)
    must("2408 AC4", "AC4", test)

    must("check_stringpool_bytes_total_lock_2408", "AC5", build)
    must("cmd_stringpool_bytes_total_lock_coverage", "AC5", build)
    must("test_stringpool_bytes_total_lock", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: stringpool string_bytes_total lock #2408 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

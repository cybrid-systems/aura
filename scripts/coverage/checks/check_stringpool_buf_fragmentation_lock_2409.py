#!/usr/bin/env python3
"""Issue #2409: buf_fragmentation single shared_lock snapshot.

Contract:
  AC1 concurrent intern + buf_fragmentation
  AC2 frag always in [0.0, 1.0]
  AC3 one shared_lock; inline string_bytes under same lock
  AC4 empty/sequential observability

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
    test = _read("tests/core/test_stringpool_buf_fragmentation_lock_2409.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate buf_fragmentation body
    idx = ast.find("buf_fragmentation() const noexcept")
    if idx < 0:
        fails.append("AC1: buf_fragmentation not found")
        body = ""
    else:
        body = ast[idx : idx + 900]

    must("Issue #2409", "AC1", ast)
    must("string_bytes_total_unlocked", "AC1", body)
    must("buf_.size()", "AC1", body)
    must("shared_lock", "AC1", body)
    must("2409 AC1", "AC1", test)

    # Must not call unlocked data_size() + separate string_bytes_total()
    if "data_size()" in body:
        fails.append("AC1: buf_fragmentation still calls data_size() (use buf_.size under lock)")
    if "string_bytes_total()" in body and "string_bytes_total_unlocked" not in body:
        fails.append("AC1: buf_fragmentation still calls string_bytes_total() (double lock)")

    must("2409 AC2", "AC2", test)
    must("0.0", "AC2", body)  # clamp / empty

    must("2409 AC3", "AC3", test)
    must("g_stringpool_intern_concurrent_readers_total", "AC3", body)

    must("2409 AC4", "AC4", test)

    must("check_stringpool_buf_fragmentation_lock_2409", "gate", build)
    must("cmd_stringpool_buf_fragmentation_lock_coverage", "gate", build)
    must("test_stringpool_buf_fragmentation_lock_2409", "gate", cmake)

    # #2408 helper still present
    must("string_bytes_total_unlocked", "lineage", ast)
    must("Issue #2408", "lineage", ast)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: stringpool buf_fragmentation lock #2409 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

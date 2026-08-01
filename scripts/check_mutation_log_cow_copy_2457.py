#!/usr/bin/env python3
"""Issue #2457: FlatAST copy shares mutation_log_ / narrowing_log_ via COW.

Contract:
  AC1 CowPmrVector type + mutation_log_ / narrowing_log_ use it
  AC2 copy ctor / assign share COW logs (documented #2457)
  AC3 test + gate wiring

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
    test = _read("tests/core/test_mutation_log_cow_copy_2457.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2457", "AC1", ast)
    must("class CowPmrVector", "AC1", ast)
    must("CowPmrVector<MutationRecord> mutation_log_", "AC1", ast)
    must("CowPmrVector<NarrowingRecord> narrowing_log_", "AC1", ast)
    must("std::shared_ptr<vector_type>", "AC1", ast)
    must("ensure_unique", "AC1", ast)

    must("share COW logs", "AC2", ast)
    # Must not deep-copy raw pmr vectors for logs on copy-assign without share
    must("mutation_log_ = other.mutation_log_", "AC2", ast)
    must("narrowing_log_ = other.narrowing_log_", "AC2", ast)

    must("2457 AC3", "AC3", test)
    must("check_mutation_log_cow_copy_2457", "gate", build)
    must("cmd_mutation_log_cow_copy_coverage", "gate", build)
    must("test_mutation_log_cow_copy_2457", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: mutation_log COW copy #2457 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

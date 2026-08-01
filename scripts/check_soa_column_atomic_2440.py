#!/usr/bin/env python3
"""Issue #2440: 4 SoA side-table columns concurrent-safe (atomic_ref + lock).

Contract:
  AC1 last_seen_epoch_ / occ_stale_ atomic_ref + dirty_column_mtx_
  AC2 verify_dirty_ / verification_dirty_ fetch_or + atomic load
  AC3 public accessors concurrent-safe; semantics preserved in test
  AC4 is_always_lock_free static_assert for uint8 + uint64

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
    test = _read("tests/core/test_soa_column_atomic_2440.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2440", "AC1", ast)
    must("std::atomic_ref<std::uint64_t>", "AC1", ast)
    must("stamp_last_seen_epoch", "AC1", ast)
    must("last_seen_epoch(NodeId id)", "AC1", ast)
    must("mark_occurrence_stale", "AC1", ast)
    must("is_occurrence_stale", "AC1", test)
    must("2440 AC1", "AC1", test)

    must("std::atomic_ref<std::uint8_t>", "AC2", ast)
    must("fetch_or(reasons", "AC2", ast)
    must("fetch_or(verify_reasons", "AC2", ast)
    must("apply_verification_dirty_bits", "AC2", ast)
    must("apply_verify_dirty_bits", "AC2", ast)
    must("2440 AC2", "AC2", test)

    must("clear_verification_dirty", "AC3", ast)
    must("is_verification_dirty", "AC3", ast)
    must("2440 AC3", "AC3", test)

    must("is_always_lock_free", "AC4", ast)
    must("std::atomic<std::uint8_t>::is_always_lock_free", "AC4", ast)
    must("std::atomic<std::uint64_t>::is_always_lock_free", "AC4", ast)
    must("2440 AC4", "AC4", test)

    must("check_soa_column_atomic_2440", "gate", build)
    must("cmd_soa_column_atomic_coverage", "gate", build)
    must("test_soa_column_atomic_2440", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: SoA column atomic #2440 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

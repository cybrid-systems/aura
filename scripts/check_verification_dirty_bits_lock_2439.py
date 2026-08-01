#!/usr/bin/env python3
"""Issue #2439: apply_verification_dirty_bits / apply_verify_dirty_bits lock.

Contract:
  AC1 exclusive dirty_column_mtx_ around newly_set RMW
  AC2 apply_verify_dirty_bits same lock pattern
  AC3 verification_dirty / verify_dirty readers shared_lock
  AC4 test + gate

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
    test = _read("tests/core/test_verification_dirty_bits_lock_2439.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2439", "AC1", ast)
    must("apply_verification_dirty_bits", "AC1", ast)
    must("dirty_column_mtx_.mutable_get()", "AC1", ast)
    must("newly_set = static_cast<std::uint8_t>(reasons & ~verification_dirty_", "AC1", ast)
    must("2439 AC1", "AC1", test)

    must("apply_verify_dirty_bits", "AC2", ast)
    must("newly_set = static_cast<std::uint8_t>(verify_reasons & ~verify_dirty_", "AC2", ast)
    must("2439 AC2", "AC2", test)

    must("verification_dirty(NodeId id)", "AC3", ast)
    must("2439 AC3", "AC3", test)

    must("2439 AC4", "AC4", test)
    must("check_verification_dirty_bits_lock_2439", "gate", build)
    must("cmd_verification_dirty_bits_lock_coverage", "gate", build)
    must("test_verification_dirty_bits_lock_2439", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: verification dirty bits lock #2439 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

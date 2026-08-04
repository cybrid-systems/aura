#!/usr/bin/env python3
"""Issue #2421: restamp_lazy_align_enabled_ is std::atomic<bool>.

Contract:
  AC1 atomic declaration (not plain bool)
  AC2 is_valid/make_ref load(acquire); wrap path store(release)
  AC3 no bare assignment of the flag
  AC4 gate + test wired

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
    test = _read("tests/core/test_restamp_lazy_align_atomic.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2421", "AC1", ast)
    must("mutable std::atomic<bool> restamp_lazy_align_enabled_{false}", "AC1", ast)
    must_not("mutable bool restamp_lazy_align_enabled_{false}", "AC1", ast)
    must("2421 AC1", "AC1", test)

    must("restamp_lazy_align_enabled_.load(std::memory_order_acquire)", "AC2", ast)
    must("restamp_lazy_align_enabled_.store(true, std::memory_order_release)", "AC2", ast)
    must("restamp_lazy_align_enabled_.store(false, std::memory_order_release)", "AC2", ast)
    must("2421 AC2", "AC2", test)

    must_not("restamp_lazy_align_enabled_ = true", "AC3", ast)
    must_not("restamp_lazy_align_enabled_ = false", "AC3", ast)
    must("2421 AC3", "AC3", test)

    must("2421 AC4", "AC4", test)
    must("check_restamp_lazy_align_atomic_2421", "gate", build)
    must("cmd_restamp_lazy_align_atomic_coverage", "gate", build)
    must("test_restamp_lazy_align_atomic", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: restamp_lazy_align atomic #2421 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

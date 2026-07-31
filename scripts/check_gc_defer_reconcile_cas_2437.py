#!/usr/bin/env python3
"""Issue #2437: reconcile_gc_defer_bits_after_clear TOCTOU / CAS fence.

Contract:
  AC1 CAS fence on depth 0→0 (skip clear if concurrent arm)
  AC2 repair re-arm Panic bit if depth > 0 after clear
  AC3 aborted counter + reconcile still fixes orphan when depth stays 0
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

    gh = _read("src/core/gc_hooks.h")
    test = _read("tests/core/test_gc_defer_reconcile_cas_2437.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2437", "AC1", gh)
    must("reconcile_gc_defer_bits_after_clear", "AC1", gh)
    must("compare_exchange_strong", "AC1", gh)
    must("2437 AC1", "AC1", test)

    # AC2
    must("arm_defer(GcDeferReason::Panic)", "AC2", gh)
    must("g_gc_defer_bit_reconcile_aborted_total", "AC2", gh)
    must("2437 AC2", "AC2", test)

    # AC3
    must("g_gc_defer_bit_reconcile_total", "AC3", gh)
    must("2437 AC4", "AC3", test)  # orphan clear path in test

    # AC4 gate
    must("2437 AC3", "AC4", test)
    must("check_gc_defer_reconcile_cas_2437", "gate", build)
    must("cmd_gc_defer_reconcile_cas_coverage", "gate", build)
    must("test_gc_defer_reconcile_cas_2437", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: gc defer reconcile cas #2437 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2429: overflow policy check+arm atomic under g_gc_defer_armed_mtx.

Contract:
  AC1 policy setters take g_gc_defer_armed_mtx; overflow path holds same lock
  AC2 concurrent set_policy + try_arm gate test
  AC3 HardFail never arms overflow under concurrent flip
  AC4 HardFail baseline semantics preserved

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

    hh = _read("src/core/gc_hooks.h")
    test = _read("tests/core/test_gc_defer_overflow_policy_atomic_2429.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2429", "AC1", hh)
    must("set_gc_defer_overflow_policy_for_test", "AC1", hh)
    # Policy setter must take armed_mtx
    must(
        "inline void set_gc_defer_overflow_policy_for_test(GcDeferOverflowPolicy p) noexcept {\n"
        "    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);",
        "AC1",
        hh,
    )
    must("check + arm/reject is atomic", "AC1", hh)
    must("2429 AC1", "AC1", test)

    must("2429 AC2", "AC2", test)
    must("concurrent policy flip", "AC2", test)

    must("2429 AC3", "AC3", test)
    must("false return never marks evaluator deferred", "AC3", test)

    must("2429 AC4", "AC4", test)
    must("HardFail overflow → false", "AC4", test)

    must("check_gc_defer_overflow_policy_atomic_2429", "gate", build)
    must("cmd_gc_defer_overflow_policy_atomic_coverage", "gate", build)
    must("test_gc_defer_overflow_policy_atomic_2429", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: gc defer overflow policy atomic #2429 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

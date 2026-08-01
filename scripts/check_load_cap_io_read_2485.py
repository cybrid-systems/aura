#!/usr/bin/env python3
"""Issue #2485: load requires kCapIoRead (capability bypass closed).

Contract:
  AC1 kCapIoRead gate + sandbox_mode in load body
  AC2 #2485 cite + denial message
  AC3 path deny list (/proc/self/mem)
  AC4 test present
  AC5 gate wiring

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

    src = _read("src/compiler/evaluator_primitives_eval.cpp")
    test = _read("tests/compiler/test_load_cap_io_read_2485.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = src.find('add("load"')
    start = max(0, idx - 350) if idx >= 0 else 0
    body = src[start : idx + 2000] if idx >= 0 else ""

    must("Issue #2485", "AC1", src)
    must("kCapIoRead", "AC1", body)
    must("sandbox_mode", "AC1", body)
    must("io-read required for load", "AC1", body)

    must("Issue #2485", "AC2", body)
    must("2485 AC1", "AC2", test)
    must("capability denied", "AC2", test.lower())

    must("/proc/self/mem", "AC3", body)
    must("kDenied", "AC3", body)

    must("test_load_cap_io_read_2485", "AC4", test)
    must("kCapIoRead", "AC4", test)

    must("check_load_cap_io_read_2485", "gate", build)
    must("cmd_load_cap_io_read_coverage", "gate", build)
    must("test_load_cap_io_read_2485", "gate", cmake)
    must("2485 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: load kCapIoRead #2485 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

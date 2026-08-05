#!/usr/bin/env python3
"""Issue #2655: sub-second denseness clocks (current-time-ms / monotonic-ms).

Contract:
  AC1 current-time-ms + monotonic-ms registered in misc
  AC2 chrono system_clock + steady_clock
  AC3 builtins parity + macro hygienic builtins list
  AC4 datetime stdlib wrappers (timestamp-ms / steady-ms / elapsed-ms)
  AC5 unit test + cmake + build.py gate

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    misc = _read("src/compiler/evaluator_primitives_misc.cpp")
    builtins = _read("src/compiler/evaluator_primitives_builtins.cpp")
    macro = _read("src/compiler/macro_expansion.cpp")
    dt = _read("lib/std/datetime.aura")
    test = _read("tests/compiler/test_subsecond_clock.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2655" in misc, "AC1: misc cites #2655")
    must('add("current-time-ms"' in misc, "AC1: current-time-ms")
    must('add("monotonic-ms"' in misc, "AC1: monotonic-ms")

    must("system_clock" in misc, "AC2: system_clock")
    must("steady_clock" in misc, "AC2: steady_clock")
    must("milliseconds" in misc, "AC2: milliseconds cast")

    must("current-time-ms" in builtins, "AC3: builtins current-time-ms")
    must("monotonic-ms" in builtins, "AC3: builtins monotonic-ms")
    must("current-time-ms" in macro, "AC3: macro hygiene list")

    must("timestamp-ms" in dt, "AC4: datetime timestamp-ms")
    must("steady-ms" in dt or "monotonic-ms" in dt, "AC4: datetime steady/monotonic")
    must("elapsed-ms" in dt, "AC4: datetime elapsed-ms")
    must("#2655" in dt, "AC4: datetime cites #2655")

    must("test_subsecond_clock" in cmake, "AC5: cmake")
    must("check_subsecond_clock_2655" in build, "AC5: linter")
    must("cmd_subsecond_clock_coverage" in build, "AC5: coverage cmd")
    must("AC1" in test and "AC3" in test and "#2655" in test, "AC5: unit ACs")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2655 sub-second denseness clocks — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

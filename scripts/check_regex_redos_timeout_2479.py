#!/usr/bin/env python3
"""Issue #2479: regex-* ReDoS wall-clock timeout + size caps.

Contract:
  AC1 run_regex_timed + AURA_REGEX_TIMEOUT_MS
  AC2 all four primitives use timed path / size limit
  AC3 regex_timeout_total metric + bump
  AC4 timeout error message
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

    math = _read("src/compiler/evaluator_primitives_math.cpp")
    met = _read("src/compiler/observability_metrics.h")
    ixx = _read("src/compiler/evaluator.ixx")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_regex_redos_timeout_2479.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2479", "AC1", math)
    must("run_regex_timed", "AC1", math)
    must("AURA_REGEX_TIMEOUT_MS", "AC1", math)
    must("AURA_REGEX_MAX_INPUT", "AC1", math)

    for prim in ("regex-match?", "regex-find", "regex-replace", "regex-split"):
        must(prim, "AC2", math)
    must("regex input exceeds size limit", "AC2", math)
    must("run_regex_timed", "AC2", math)

    must("regex_timeout_total", "AC3", met)
    must("bump_regex_timeout_total", "AC3", ixx)
    must("regex-timeouts", "AC3", q)
    must("schema-2479", "AC3", q)

    must("regex execution exceeded timeout", "AC4", math)
    must("2479 AC2", "AC4", test)

    must("check_regex_redos_timeout_2479", "gate", build)
    must("cmd_regex_redos_timeout_coverage", "gate", build)
    must("test_regex_redos_timeout_2479", "gate", cmake)
    must("2479 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: regex ReDoS timeout #2479 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

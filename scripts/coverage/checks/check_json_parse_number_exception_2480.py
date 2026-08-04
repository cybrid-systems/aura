#!/usr/bin/env python3
"""Issue #2480: json-parse parse_number catches stod/stoll exceptions.

Contract:
  AC1 try/catch out_of_range + invalid_argument
  AC2 make_primitive_error messages
  AC3 register_json wires error_values
  AC4 SILENCE-PRIM + #2480 cite
  AC5 gate wiring

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

    js = _read("src/compiler/evaluator_primitives_json.cpp")
    reg = _read("src/compiler/evaluator_primitives_registry.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_json_parse_number_exception.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pidx = js.find("auto parse_number")
    # try/catch is ~1.2k into the lambda body
    body = js[pidx : pidx + 2200] if pidx >= 0 else ""

    must("Issue #2480", "AC1", js)
    must("try", "AC1", body)
    must("out_of_range", "AC1", body)
    must("invalid_argument", "AC1", body)

    must("make_primitive_error", "AC2", body)
    must("number out of range", "AC2", body)
    must("invalid number format", "AC2", body)

    must("error_values", "AC3", js)
    must("primitive_error_counter", "AC3", js)
    must("error_values_", "AC3", reg)
    must("error_values", "AC3", ixx)

    must("SILENCE-PRIM", "AC4", body)
    must("2480 AC1", "AC4", test)

    must("check_json_parse_number_exception_2480", "gate", build)
    must("cmd_json_parse_number_exception_coverage", "gate", build)
    must("test_json_parse_number_exception", "gate", cmake)
    must("2480 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: json-parse number exception safety #2480 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

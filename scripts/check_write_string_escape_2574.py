#!/usr/bin/env python3
"""Issue #2574: Scheme write string escape (JIT + TW).

Contract:
  AC1 aura_display_value write_mode uses escape helper
  AC2 TW io_print_val quote path escapes similarly
  AC3 display path stays raw (fputs / %s without escape)
  AC4 regression test + cmake + build.py gate

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

    jit = _read("src/compiler/aura_jit_runtime.cpp")
    tw = _read("src/compiler/evaluator_primitives_runtime.cpp")
    test = _read("tests/compiler/test_write_string_escape_2574.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2574", "AC1", jit)
    must("fputs_scheme_write_string", "AC1", jit)
    must('\\\\"', "AC1", jit)

    must("#2574", "AC2", tw)
    must('\\\\"', "AC2", tw)

    must("fputs(s, stdout)", "AC3", jit)
    must('fprintf(stdout, "%s"', "AC3", tw)

    must("ac1_write_embedded_quote", "AC4", test)
    must("test_write_string_escape_2574", "AC4", cmake)
    must("check_write_string_escape_2574", "AC4", build)
    must("cmd_write_string_escape_coverage", "AC4", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2574 write string escape — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

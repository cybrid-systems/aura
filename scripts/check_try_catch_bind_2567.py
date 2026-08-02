#!/usr/bin/env python3
"""Issue #2567: try/catch binds catch parameter for handler use.

Contract:
  AC1 Diagnostic unexpected → first-class string bind
  AC2 (error …) cause bound (not opaque RefError short-circuit)
  AC3 bare Variable catch binding
  AC4 source-cite #2567 in eval_flat (+ lowering)
  AC5 test + cmake + build.py gate

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    low = _read("src/compiler/lowering_impl.cpp")
    test = _read("tests/compiler/test_try_catch_bind_2567.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2567", "AC1", flat)
    must("result.error()", "AC1", flat)
    must("ac1_diagnostic_bind", "AC1", test)

    # AC2
    must("as_error_idx", "AC2", flat)
    must("error_values_", "AC2", flat)
    must("ac2_error_prim_bind", "AC2", test)

    # AC3
    must("bare var form", "AC3", flat)
    must("NodeTag::Variable", "AC3", flat)
    must("ac4_bare_var", "AC3", test)

    # AC4
    must("#2567", "AC4", flat)
    must("#2567", "AC4", low)
    must("ac5_source_gate", "AC4", test)

    # AC5
    must("test_try_catch_bind_2567", "AC5", cmake)
    must("check_try_catch_bind_2567", "AC5", build)
    must("cmd_try_catch_bind_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2567 try/catch bind — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2407: AOT move/Linear of Copy literals as no-ops + emit-binary link.

Contract:
  AC1–3 emit:move-int / emit:linear / emit:lin-drop re-enabled in run-tests.sh
  AC4 no hard-fail on move of non-linear / elide literal Move at lower
  AC5 drop/borrow emit tests remain enabled
  AC6 runtime.c weak pin/unpin stubs for residual Linear* AOT emit
  AC7 source-cite + build gate

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
            fails.append(f"{label}: unexpected {n!r} still present")

    rt = _read("lib/runtime.c")
    lower = _read("src/compiler/lowering_linear_types_impl.cpp")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    sh = _read("tests/python/run-tests.sh")
    build = _read("build.py")

    # AC1–3 re-enabled
    must('run_emit_test "emit:move-int"', "AC1", sh)
    must('run_emit_test "emit:linear"', "AC2", sh)
    must('run_emit_test "emit:lin-drop"', "AC3", sh)
    must_not('#run_emit_test "emit:move-int"', "AC1", sh)
    must_not('#run_emit_test "emit:linear"', "AC2", sh)
    must_not('#run_emit_test "emit:lin-drop"', "AC3", sh)

    # AC4 / lowering elision
    must("Issue #2407", "AC4", lower)
    must("copy_literal", "AC4", lower)
    must("LiteralInt", "AC4", lower)
    must("Issue #2407", "AC4", ir)
    must("!types::is_linear(val)", "AC4", ir)

    # AC5 drop/borrow still enabled
    must('run_emit_test "emit:drop-int"', "AC5", sh)
    must('run_emit_test "emit:borrow"', "AC5", sh)
    must('run_emit_test "emit:drop-pair"', "AC5", sh)

    # AC6 runtime stubs
    must("aura_jit_pin_linear_root", "AC6", rt)
    must("aura_jit_unpin_linear_root", "AC6", rt)
    must("Issue #2407", "AC6", rt)
    must("__attribute__((weak))", "AC6", rt)

    # AC7 gate wiring
    must("check_aot_linear_literal_noop_2407", "AC7", build)
    must("cmd_aot_linear_literal_noop_coverage", "AC7", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: AOT linear literal no-op #2407 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2575: dual string heaps — PrimCall re-interns eval→JIT.

Contract:
  AC1 convert_str_for_jit + convert_str_for_eval in service.ixx
  AC2 aura_jit_prim_dispatch uses both at PrimCall boundary
  AC3 aura_display_value documents dual-heap / ABI
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

    svc = _read("src/compiler/service.ixx")
    jit_rt = _read("src/compiler/aura_jit_runtime.cpp")
    jit = _read("src/compiler/aura_jit.cpp")
    test = _read("tests/compiler/test_jit_dual_string_heap_2575.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2575", "AC1", svc)
    must("convert_str_for_jit", "AC1", svc)
    must("convert_str_for_eval", "AC1", svc)
    must("aura_alloc_string", "AC1", svc)

    must("aura_jit_prim_dispatch", "AC2", svc)
    must("convert_str_for_jit", "AC2", svc)

    must("#2575", "AC3", jit_rt)
    must("int64_t write_mode", "AC3", jit_rt)
    must("aura_display_value(int64_t, int64_t)", "AC3", jit)

    must("ac1_string_append_display", "AC4", test)
    must("test_jit_dual_string_heap_2575", "AC4", cmake)
    must("check_jit_dual_string_heap_2575", "AC4", build)
    must("cmd_jit_dual_string_heap_coverage", "AC4", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2575 dual string heap — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

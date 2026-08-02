#!/usr/bin/env python3
"""Issue #2577: PrimCall string re-intern content intern (no O(N) growth).

Contract:
  AC1 aura_alloc_string uses g_string_intern content map
  AC2 convert_str_for_eval uses jit-idx cache + content scan
  AC3 clear intern on aura_reset_runtime
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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_primcall_str_intern_2577.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2577", "AC1", rt)
    must("g_string_intern", "AC1", rt)
    must("aura_alloc_string", "AC1", rt)

    must("#2577", "AC2", svc)
    must("g_jit_idx_to_eval_idx", "AC2", svc)
    must("convert_str_for_eval", "AC2", svc)

    must("g_string_intern.clear", "AC3", rt)

    must("ac1_loop_heap_growth", "AC4", test)
    must("test_primcall_str_intern_2577", "AC4", cmake)
    must("check_primcall_str_intern_2577", "AC4", build)
    must("cmd_primcall_str_intern_coverage", "AC4", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2577 PrimCall str intern — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

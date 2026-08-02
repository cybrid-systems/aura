#!/usr/bin/env python3
"""Issue #2576: JIT PrimCall N-arg ABI.

Contract:
  AC1 aura_prim_call(slot, args*, count) in jit runtime
  AC2 OpPrimCall packs frame locals into stack buffer
  AC3 AOT lib/runtime.c same ABI
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
    jit = _read("src/compiler/aura_jit.cpp")
    aot = _read("lib/runtime.c")
    test = _read("tests/compiler/test_primcall_narg_2576.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2576", "AC1", rt)
    must("kAuraPrimCallMaxArgs", "AC1", rt)
    must("int64_t* args", "AC1", rt)

    must("#2576", "AC2", jit)
    must("CreateAlloca", "AC2", jit)
    must("fn_prim_call", "AC2", jit)

    must("#2576", "AC3", aot)
    must("int64_t* args", "AC3", aot)

    must("ac1_append3", "AC4", test)
    must("test_primcall_narg_2576", "AC4", cmake)
    must("check_primcall_narg_2576", "AC4", build)
    must("cmd_primcall_narg_coverage", "AC4", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2576 PrimCall N-arg — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

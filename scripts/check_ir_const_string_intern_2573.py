#!/usr/bin/env python3
"""Issue #2573: IR ConstString intern — no O(N) string_heap growth.

Contract:
  AC1 ir_executor ConstString uses const_string_cache_ by pool index
  AC2 cache member declared on IRInterpreter
  AC3 regression test + cmake + build.py gate
  AC4 #2573 cites on impl + header

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

    impl = _read("src/compiler/ir_executor_impl.cpp")
    hdr = _read("src/compiler/ir_executor.ixx")
    test = _read("tests/compiler/test_ir_const_string_intern_2573.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2573", "AC1", impl)
    must("const_string_cache_", "AC1", impl)
    must("pool_idx", "AC1", impl)

    must("#2573", "AC2", hdr)
    must("const_string_cache_", "AC2", hdr)

    must("ac1_loop_heap_growth_bounded", "AC3", test)
    must("test_ir_const_string_intern_2573", "AC3", cmake)
    must("check_ir_const_string_intern_2573", "AC3", build)
    must("cmd_ir_const_string_intern_coverage", "AC3", build)

    must("AURA_FORCE_IR", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2573 IR ConstString intern — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2806: clone_macro_body recursion depth is explicit (not TLS race).

Contract (one row per AC):
  AC1 clone_macro_body_at_depth + hygiene_depth; cross_flat_top uses depth==0
  AC2 concurrent_top_level metric + v_read + recursion depth+1
  AC3 tests/compiler/test_concurrent_clone_hygiene_depth.cpp
  AC4 this linter wired; no docs/design/2806-*; no test_issue_2806.cpp

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

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    bridge = _read("src/compiler/aura_jit_bridge.h")
    test = _read("tests/compiler/test_concurrent_clone_hygiene_depth.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2806", "AC1", me)
    must("clone_macro_body_at_depth", "AC1", me)
    must("hygiene_depth", "AC1", me)
    cpos = me.find("cross_flat_top")
    cwin = me[cpos : cpos + 400] if cpos >= 0 else ""
    if "hygiene_depth == 0" not in cwin and "hygiene_depth==0" not in cwin:
        fails.append("AC1: cross_flat_top does not use hygiene_depth == 0")

    # AC2
    must("g_clone_macro_body_concurrent_top_level_total", "AC2", me)
    must("g_clone_macro_body_concurrent_top_level_total", "AC2", ixx)
    must("aura_clone_macro_body_concurrent_top_level_total_v_read", "AC2", me)
    must("aura_clone_macro_body_concurrent_top_level_total_v_read", "AC2", bridge)
    if "hygiene_depth + 1" not in me and "hygiene_depth+1" not in me:
        fails.append("AC2: recursion must pass hygiene_depth + 1")

    # AC3
    must("ac2806", "AC3", test)
    must("2806", "AC3", test)
    must("clone_macro_body_at_depth", "AC3", test)
    must("std::thread", "AC3", test)
    must("concurrent", "AC3", test.lower())
    if not (ROOT / "tests" / "compiler" / "test_concurrent_clone_hygiene_depth.cpp").is_file():
        fails.append("AC3: missing test_concurrent_clone_hygiene_depth.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2806.cpp").is_file():
        fails.append("AC3: test_issue_2806.cpp present (forbidden per #81967)")
    must("test_concurrent_clone_hygiene_depth", "AC3", cmake)

    # AC4
    must("check_concurrent_clone_hygiene_depth_2806", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2806-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2806 concurrent clone hygiene depth — explicit depth param + concurrent top-level metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2804: clone-walk rename_binding enforces s_max_gensym_map_size.

Symmetric with rename_binding_pre; clone_walk_gensym_ceiling_exceeded metric.

Contract (one row per AC):
  AC1 rename_binding cites #2804; s_max_gensym_map_size + clone_walk metric
  AC2 g_clone_walk_gensym_ceiling_exceeded_total export + v_read + test setter
  AC3 tests/compiler/test_clone_walk_gensym_ceiling.cpp
  AC4 this linter wired; no docs/design/2804-*; no test_issue_2804.cpp

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
    test = _read("tests/compiler/test_clone_walk_gensym_ceiling.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pre = me.find("auto rename_binding_pre")
    walk = me.find("auto rename_binding =", pre + 1 if pre >= 0 else 0)
    win = me[walk : walk + 1200] if walk >= 0 else ""

    # AC1
    must("Issue #2804", "AC1", win)
    if "effective_max_gensym_map_size" not in win and "gensym_cap" not in win and "s_max_gensym_map_size" not in win:
        fails.append("AC1: missing ceiling check (effective_max / gensym_cap / s_max)")
    must("g_clone_walk_gensym_ceiling_exceeded_total", "AC1", win)
    must("g_macro_self_evo_gensym_map_size_exceeded_total", "AC1", win)

    # AC2
    must("g_clone_walk_gensym_ceiling_exceeded_total", "AC2", me)
    must("g_clone_walk_gensym_ceiling_exceeded_total", "AC2", ixx)
    must("aura_clone_walk_gensym_ceiling_exceeded_total_v_read", "AC2", me)
    must("aura_clone_walk_gensym_ceiling_exceeded_total_v_read", "AC2", bridge)
    must("aura_test_set_max_gensym_map_size_for_test", "AC2", me)
    must("aura_test_set_max_gensym_map_size_for_test", "AC2", bridge)

    # AC3
    must("ac2804", "AC3", test)
    must("2804", "AC3", test)
    must("rename_binding", "AC3", test)
    must("clone_macro_body", "AC3", test)
    must("name_map.size()", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_clone_walk_gensym_ceiling.cpp").is_file():
        fails.append("AC3: missing test_clone_walk_gensym_ceiling.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2804.cpp").is_file():
        fails.append("AC3: test_issue_2804.cpp present (forbidden per #81967)")
    must("test_clone_walk_gensym_ceiling", "AC3", cmake)

    # AC4
    must("check_clone_walk_gensym_ceiling_2804", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2804-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2804 clone-walk gensym ceiling — rename_binding parity with rename_binding_pre")
    return 0


if __name__ == "__main__":
    sys.exit(main())

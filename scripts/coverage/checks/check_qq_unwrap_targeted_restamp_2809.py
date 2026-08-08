#!/usr/bin/env python3
"""Issue #2809: expand_inner_macros qq-unwrap uses targeted restamp (not full).

Contract (one row per AC):
  AC1 expand_inner_macros cites #2809; restamp_after_qq_unwrap; no full restamp
  AC2 targeted/full metrics + v_read + FlatAST restamp_node_generation
  AC3 tests/compiler/test_qq_unwrap_targeted_restamp.cpp
  AC4 this linter wired; no docs/design/2809-*; no test_issue_2809.cpp

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
            fails.append(f"{label}: forbidden {n!r} present")

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    bridge = _read("src/compiler/aura_jit_bridge.h")
    ast_ixx = _read("src/core/ast.ixx")
    ast_impl = _read("src/core/ast_impl.cpp")
    test = _read("tests/compiler/test_qq_unwrap_targeted_restamp.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = me.find("static void restamp_after_qq_unwrap")
    # Include preceding Issue #2809 contract comment block (~960 chars).
    win_start = max(0, pos - 1200) if pos >= 0 else 0
    win = me[win_start : pos + 1600] if pos >= 0 else ""
    eim = me.find("aura::ast::NodeId expand_inner_macros")
    eim_win = me[eim : eim + 1800] if eim >= 0 else ""

    # AC1
    must("Issue #2809", "AC1", win)
    must("restamp_after_qq_unwrap", "AC1", me)
    must("restamp_node_generation", "AC1", win)
    must("restamp_subtree_generation", "AC1", win)
    must("g_macro_expand_targeted_restamp_total", "AC1", win)
    must("restamp_after_qq_unwrap", "AC1", eim_win)
    # Forbid a live call; comments may still mention the old API name.
    must_not("->restamp_all_node_generations", "AC1", eim_win)
    must_not(".restamp_all_node_generations()", "AC1", eim_win)

    # AC2
    must("g_macro_expand_targeted_restamp_total", "AC2", me)
    must("g_macro_expand_full_restamp_total", "AC2", me)
    must("g_macro_expand_targeted_restamp_total", "AC2", ixx)
    must("g_macro_expand_full_restamp_total", "AC2", ixx)
    must("aura_macro_expand_targeted_restamp_total_v_read", "AC2", me)
    must("aura_macro_expand_full_restamp_total_v_read", "AC2", me)
    must("aura_macro_expand_targeted_restamp_total_v_read", "AC2", bridge)
    must("aura_macro_expand_full_restamp_total_v_read", "AC2", bridge)
    must("restamp_node_generation", "AC2", ast_ixx)
    must("enable_restamp_lazy_align", "AC2", ast_ixx)
    must("restamp_node_generation", "AC2", ast_impl)

    # AC3
    must("ac2809", "AC3", test)
    must("2809", "AC3", test)
    must("restamp_after_qq_unwrap", "AC3", test)
    must("g_macro_expand_targeted_restamp_total", "AC3", test)
    must("expand_inner_macros", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_qq_unwrap_targeted_restamp.cpp").is_file():
        fails.append("AC3: missing test_qq_unwrap_targeted_restamp.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2809.cpp").is_file():
        fails.append("AC3: test_issue_2809.cpp present (forbidden per #81967)")
    must("test_qq_unwrap_targeted_restamp", "AC3", cmake)

    # AC4
    must("check_qq_unwrap_targeted_restamp_2809", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2809-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2809 qq-unwrap targeted restamp — O(|unwrapped|) not O(N×M)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

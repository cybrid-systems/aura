#!/usr/bin/env python3
"""Issue #2805: dotted-rest fallback must not rename hygiene_builtins via name_map.

Contract (one row per AC):
  AC1 Lambda fallback cites #2805; hygiene_builtins + prevented metric
  AC2 g_dotted_rest_builtin_rename_prevented_total export + v_read
  AC3 tests/compiler/test_dotted_rest_builtin_rename.cpp
  AC4 this linter wired; no docs/design/2805-*; no test_issue_2805.cpp

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
    test = _read("tests/compiler/test_dotted_rest_builtin_rename.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    fb = me.find("__rest_fb_")
    win = me[max(0, fb - 800) : fb + 900] if fb >= 0 else ""

    # AC1
    must("Issue #2805", "AC1", win)
    must("hygiene_builtins", "AC1", win)
    must("g_dotted_rest_builtin_rename_prevented_total", "AC1", win)

    # AC2
    must("g_dotted_rest_builtin_rename_prevented_total", "AC2", me)
    must("g_dotted_rest_builtin_rename_prevented_total", "AC2", ixx)
    must("aura_dotted_rest_builtin_rename_prevented_total_v_read", "AC2", me)
    must("aura_dotted_rest_builtin_rename_prevented_total_v_read", "AC2", bridge)

    # AC3
    must("ac2805", "AC3", test)
    must("2805", "AC3", test)
    must("hygiene_builtins", "AC3", test)
    must("clone_macro_body", "AC3", test)
    must("name_map", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_dotted_rest_builtin_rename.cpp").is_file():
        fails.append("AC3: missing test_dotted_rest_builtin_rename.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2805.cpp").is_file():
        fails.append("AC3: test_issue_2805.cpp present (forbidden per #81967)")
    must("test_dotted_rest_builtin_rename", "AC3", cmake)

    # AC4
    must("check_dotted_rest_builtin_rename_2805", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2805-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2805 dotted-rest builtin rename prevented — hygiene_builtins guard + metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())

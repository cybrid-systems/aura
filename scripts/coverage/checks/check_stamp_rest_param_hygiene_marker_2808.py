#!/usr/bin/env python3
"""Issue #2808: stamp_rest_param_hygiene sets SyntaxMarker::MacroIntroduced.

Contract (one row per AC):
  AC1 stamp_rest_param_hygiene cites #2808; set_marker MacroIntroduced + metrics
  AC2 marker set/skipped totals export + v_read + test call entry
  AC3 tests/compiler/test_stamp_rest_param_hygiene_marker.cpp
  AC4 this linter wired; no docs/design/2808-*; no test_issue_2808.cpp

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
    test = _read("tests/compiler/test_stamp_rest_param_hygiene_marker.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = me.find("static inline void stamp_rest_param_hygiene")
    win = me[pos : pos + 2000] if pos >= 0 else ""

    # AC1
    must("Issue #2808", "AC1", win)
    must("set_marker", "AC1", win)
    must("MacroIntroduced", "AC1", win)
    must("g_stamp_rest_param_marker_set_total", "AC1", win)
    must("g_stamp_rest_param_marker_skipped_total", "AC1", win)

    # AC2
    must("g_stamp_rest_param_marker_set_total", "AC2", me)
    must("g_stamp_rest_param_marker_set_total", "AC2", ixx)
    must("aura_stamp_rest_param_marker_set_total_v_read", "AC2", me)
    must("aura_stamp_rest_param_marker_set_total_v_read", "AC2", bridge)
    must("aura_test_call_stamp_rest_param_hygiene", "AC2", me)
    must("aura_test_call_stamp_rest_param_hygiene", "AC2", bridge)

    # AC3
    must("ac2808", "AC3", test)
    must("2808", "AC3", test)
    must("is_macro_introduced", "AC3", test)
    must("stamp_rest_param", "AC3", test)
    must("MacroIntroduced", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_stamp_rest_param_hygiene_marker.cpp").is_file():
        fails.append("AC3: missing test_stamp_rest_param_hygiene_marker.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2808.cpp").is_file():
        fails.append("AC3: test_issue_2808.cpp present (forbidden per #81967)")
    must("test_stamp_rest_param_hygiene_marker", "AC3", cmake)

    # AC4
    must("check_stamp_rest_param_hygiene_marker_2808", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2808-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2808 stamp_rest_param_hygiene marker — MacroIntroduced + set/skipped metrics")
    return 0


if __name__ == "__main__":
    sys.exit(main())

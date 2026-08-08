#!/usr/bin/env python3
"""Issue #2807: pre_scan treats unquote-splicing as caller-scope (like unquote).

Contract (one row per AC):
  AC1 pre_scan cites #2807; unquote-splicing + mismatch metric
  AC2 metric export + v_read + test reset
  AC3 tests/compiler/test_unquote_splicing_hygiene.cpp
  AC4 this linter wired; no docs/design/2807-*; no test_issue_2807.cpp

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
    test = _read("tests/compiler/test_unquote_splicing_hygiene.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Prefer the pre_scan handler site (cname == ...), with enough lead-in
    # for the Issue #2807 comment block.
    pos = me.find('cname == "unquote-splicing"')
    if pos < 0:
        pos = me.find("unquote-splicing")
    win = me[max(0, pos - 600) : pos + 500] if pos >= 0 else ""

    # AC1
    must("Issue #2807", "AC1", win)
    must("unquote-splicing", "AC1", win)
    must("g_unquote_splicing_hygiene_mismatch_total", "AC1", win)

    # AC2
    must("g_unquote_splicing_hygiene_mismatch_total", "AC2", me)
    must("g_unquote_splicing_hygiene_mismatch_total", "AC2", ixx)
    must("aura_unquote_splicing_hygiene_mismatch_total_v_read", "AC2", me)
    must("aura_unquote_splicing_hygiene_mismatch_total_v_read", "AC2", bridge)

    # AC3
    must("ac2807", "AC3", test)
    must("2807", "AC3", test)
    must("unquote-splicing", "AC3", test)
    must("nested_qq", "AC3", test)
    must("clone_macro_body", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_unquote_splicing_hygiene.cpp").is_file():
        fails.append("AC3: missing test_unquote_splicing_hygiene.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2807.cpp").is_file():
        fails.append("AC3: test_issue_2807.cpp present (forbidden per #81967)")
    must("test_unquote_splicing_hygiene", "AC3", cmake)

    # AC4
    must("check_unquote_splicing_hygiene_2807", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2807-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2807 unquote-splicing hygiene — pre_scan caller-scope boundary + mismatch metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())

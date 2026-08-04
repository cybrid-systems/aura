#!/usr/bin/env python3
"""Issue #2392: fixup_deltas safe rebase (bounds + overflow → NULL_NODE).

Contract:
  AC1 Valid deltas → absolute children
  AC2 Over-large / overflow deltas clamp to NULL_NODE
  AC3 Documented relative-delta model; no unchecked cid+id alone
  AC4 Tests + CMake + build.py gate
  AC5 No hard-abort on fixup path

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    impl = _read("src/core/ast_impl.cpp")
    ixx = _read("src/core/ast.ixx")
    test = _read("tests/core/test_fixup_deltas_2392.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1–AC3 implementation
    must("Issue #2392", "AC1", impl)
    must("fixup_deltas", "AC1", impl)
    must("NULL_NODE", "AC2", impl)
    must("delta", "AC3", impl)
    # Must not be the old one-liner alone: set_child(id, j, cid + id)
    if re.search(r"set_child\s*\(\s*id\s*,\s*j\s*,\s*cid\s*\+\s*id\s*\)", impl):
        fails.append("AC3: still has unchecked set_child(id, j, cid + id)")
    must("ac1_valid_deltas", "AC1", test)
    must("ac2_oob_and_overflow", "AC2", test)
    must("ac3_parent_zero_absolute", "AC3", test)

    # AC4 registration
    must("2392", "AC4", ixx)
    must("test_fixup_deltas_2392", "AC4", cmake)
    must("check_fixup_deltas_2392", "AC4", build)
    must("cmd_fixup_deltas_coverage", "AC4", build)
    must("ac4_source_and_gate", "AC4", test)
    must("Issue #2392", "AC4", test)

    # AC5: no abort in fixup_deltas body
    start = impl.find("void fixup_deltas(FlatAST& ast)")
    if start < 0:
        fails.append("AC5: fixup_deltas definition not found")
    else:
        body = impl[start : start + 2500]
        if "std::abort()" in body or "abort()" in body:
            fails.append("AC5: fixup_deltas still aborts")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2392 fixup_deltas safe rebase — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

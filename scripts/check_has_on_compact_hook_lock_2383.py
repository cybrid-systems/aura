#!/usr/bin/env python3
"""Issue #2383: has_on_compact_hook locks hook_mtx_ (parity with layout/root_remap).

Contract:
  AC1 All three has_* take their respective mutexes
  AC2 Concurrent set+has stress test surface
  AC3 Semantics + CMake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _method_body(src: str, name: str) -> str:
    m = re.search(rf"bool\s+{re.escape(name)}\(\)\s+const\s+noexcept\s*\{{", src)
    if not m:
        return ""
    i = m.end() - 1  # at '{'
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i : j + 1]
    return ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_has_on_compact_hook_lock_2383.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2383", "AC1", arena)
    compact = _method_body(arena, "has_on_compact_hook")
    layout = _method_body(arena, "has_on_layout_change")
    root = _method_body(arena, "has_root_remap_callback")
    if not compact:
        fails.append("AC1: has_on_compact_hook body not found")
    else:
        if "hook_mtx_" not in compact:
            fails.append("AC1: has_on_compact_hook missing hook_mtx_")
        if "lock_guard" not in compact:
            fails.append("AC1: has_on_compact_hook missing lock_guard")
    if not layout or "on_layout_change_mtx_" not in layout:
        fails.append("AC1: has_on_layout_change missing on_layout_change_mtx_")
    if not root or "root_remap_mtx_" not in root:
        fails.append("AC1: has_root_remap_callback missing root_remap_mtx_")
    must("ac1_source_lock_parity", "AC1", test)

    # AC2
    must("ac2_concurrent_set_has", "AC2", test)
    must("has_on_compact_hook", "AC2", test)
    must("set_on_compact_hook", "AC2", test)

    # AC3
    must("ac3_semantics", "AC3", test)
    must("test_has_on_compact_hook_lock_2383", "AC3", cmake)
    must("check_has_on_compact_hook_lock_2383", "AC3", build)
    must("cmd_has_on_compact_hook_lock_coverage", "AC3", build)
    must("Issue #2383", "AC3", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2383 has_on_compact_hook lock parity — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

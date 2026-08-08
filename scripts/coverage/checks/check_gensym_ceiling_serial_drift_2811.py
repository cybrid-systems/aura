#!/usr/bin/env python3
"""Issue #2811: rename_binding_pre does not advance hyg_ctr on ceiling deny.

Contract (one row per AC):
  AC1 rename_binding_pre cites #2811; ceiling before hyg_ctr++; drift metric
  AC2 g_gensym_serial_drift_total + v_read + test reset
  AC3 tests/compiler/test_gensym_ceiling_serial_drift.cpp
  AC4 this linter wired; no docs/design/2811-*; no test_issue_2811.cpp

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
    test = _read("tests/compiler/test_gensym_ceiling_serial_drift.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pre = me.find("auto rename_binding_pre")
    win = me[pre : pre + 2500] if pre >= 0 else ""

    # AC1
    must("Issue #2811", "AC1", win)
    must("g_gensym_serial_drift_total", "AC1", win)
    must("std::to_string(hyg_ctr++)", "AC1", win)
    must("gensym_cap", "AC1", win)
    # Ceiling check must precede the serial-consume expression (not comments).
    ceil = win.find("const auto gensym_cap")
    hyg = win.find("std::to_string(hyg_ctr++)")
    if ceil < 0 or hyg < 0 or ceil >= hyg:
        fails.append("AC1: ceiling check must appear before std::to_string(hyg_ctr++) in rename_binding_pre")

    # AC2
    must("g_gensym_serial_drift_total", "AC2", me)
    must("g_gensym_serial_drift_total", "AC2", ixx)
    must("aura_gensym_serial_drift_total_v_read", "AC2", me)
    must("aura_gensym_serial_drift_total_v_read", "AC2", bridge)
    must("aura_test_reset_gensym_serial_drift_total_for_test", "AC2", me)
    must("aura_test_reset_gensym_serial_drift_total_for_test", "AC2", bridge)

    # AC3
    must("ac2811", "AC3", test)
    must("2811", "AC3", test)
    must("g_gensym_serial_drift_total", "AC3", test)
    must("rename_binding_pre", "AC3", test)
    must("hyg_ctr", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_gensym_ceiling_serial_drift.cpp").is_file():
        fails.append("AC3: missing test_gensym_ceiling_serial_drift.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2811.cpp").is_file():
        fails.append("AC3: test_issue_2811.cpp present (forbidden per #81967)")
    must("test_gensym_ceiling_serial_drift", "AC3", cmake)

    # AC4
    must("check_gensym_ceiling_serial_drift_2811", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2811-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2811 gensym ceiling serial drift — hyg_ctr only advances on success")
    return 0


if __name__ == "__main__":
    sys.exit(main())

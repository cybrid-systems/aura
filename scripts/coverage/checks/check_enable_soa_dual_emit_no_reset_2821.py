#!/usr/bin/env python3
"""Issue #2821: enable_soa_dual_emit skips module_v2 wipe when already on.

Contract (one row per AC):
  AC1 enable cites #2821; force_reset; skip-reset metric
  AC2 test preserves multi-function SoA on second enable
  AC3 force_reset wipes; cold first enable OK
  AC4 linter wired; schema-2821; no docs/design/2821-*; no test_issue_2821.cpp

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

    low = _read("src/compiler/lowering.ixx")
    soa = _read("src/compiler/ir_soa.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_enable_soa_dual_emit_no_reset.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = low.find("void enable_soa_dual_emit")
    body = low[pos : pos + 1200] if pos >= 0 else ""

    # AC1
    must("Issue #2821", "AC1", body)
    must("force_reset", "AC1", body)
    must("g_enable_soa_dual_emit_skip_reset_total_atomic", "AC1", body)
    must("dual_emit_soa && !force_reset", "AC1", body)
    must("module_v2 = {}", "AC1", body)
    must("g_enable_soa_dual_emit_skip_reset_total_atomic", "AC1", soa)

    # AC2 / AC3
    must("ac2821", "AC2", test)
    must("2821", "AC2", test)
    must("enable_soa_dual_emit", "AC2", test)
    must("func_a", "AC2", test)
    must("func_b", "AC2", test)
    must("force_reset", "AC3", test)
    must("g_enable_soa_dual_emit_skip_reset_total_atomic", "AC2", test)
    if not (ROOT / "tests" / "compiler" / "test_enable_soa_dual_emit_no_reset.cpp").is_file():
        fails.append("AC2: missing test_enable_soa_dual_emit_no_reset.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2821.cpp").is_file():
        fails.append("AC2: test_issue_2821.cpp present (forbidden per #81967)")
    must("test_enable_soa_dual_emit_no_reset", "AC2", cmake)

    # AC4
    must("check_enable_soa_dual_emit_no_reset_2821", "AC4", build)
    must("schema-2821", "AC4", obs)
    must("enable-soa-dual-emit-skip-reset-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2821-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2821 enable_soa_dual_emit skip-reset — no silent SoA wipe")
    return 0


if __name__ == "__main__":
    sys.exit(main())

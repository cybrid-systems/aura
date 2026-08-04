#!/usr/bin/env python3
"""Issue #2431: pure columnar DeadCoercionElimination on IRModuleV2.

Contract:
  AC1 run(IRModuleV2) uses run_columnar_block; no aos_block materialize
  AC2 run_columnar_block implements rules (Local elision / nested / Dynamic)
  AC3 metrics g_dead_coercion_columnar_total / aos_bridge_total
  AC4 gate + test wired

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
            fails.append(f"{label}: unexpected {n!r}")

    # Issue #2524: DeadCoercionEliminationPass lives in pass_impls.ixx;
    # pass_manager.ixx is a thin facade that re-exports it.
    pm = _read("src/compiler/pass_impls.ixx")
    if not pm:
        pm = _read("src/compiler/pass_manager.ixx")
    test = _read("tests/compiler/test_dead_coercion_columnar.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2431", "AC1", pm)
    must("run_columnar_block", "AC1", pm)
    must("g_dead_coercion_columnar_total", "AC1", pm)
    # No residual AoS materialize in run(IRModuleV2)
    must_not("aura::ir::BasicBlock aos_block", "AC1", pm)
    must("2431 AC1", "AC1", test)

    must("IROpcode::Local", "AC2", pm)
    must("narrow_evidence", "AC2", pm)
    must("2431 AC2", "AC2", test)

    must("g_dead_coercion_aos_bridge_total", "AC3", pm)
    must("2431 AC3", "AC3", test)

    must("2431 AC4", "AC4", test)
    must("2431 AC5", "AC5", test)
    must("check_dead_coercion_columnar_2431", "gate", build)
    must("cmd_dead_coercion_columnar_coverage", "gate", build)
    must("test_dead_coercion_columnar", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: dead coercion columnar #2431 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

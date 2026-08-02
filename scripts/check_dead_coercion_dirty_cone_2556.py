#!/usr/bin/env python3
"""Issue #2556: DeadCoercion DCE dirty-cone scan limit coverage.

Contract:
  AC1 Partial cone CastOp skip counting + dirty_cone_partial_runs
  AC2 Full scan path when no cone (full_scan_runs)
  AC3 Soft empty cone (no dirty-mask alloc before any_dirty probe)
  AC4 type_ir_union_cone_* helpers + layered schema-2556 keys
  AC5 test + cmake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    opt = _read("src/compiler/optimization_passes.ixx")
    dirty = _read("src/compiler/dirty_propagation.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_dead_coercion_dirty_cone_2556.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2556", "AC1", opt)
    must("dead_coercion_dirty_cone_partial_runs", "AC1", opt)
    must("dead_coercion_dirty_cone_cast_sites_scanned", "AC1", opt)
    must("count_cast_ops_in_block", "AC1", opt)
    must("skipped_casts", "AC1", opt)
    must("ac1_partial_cone", "AC1", test)

    # AC2
    must("dead_coercion_full_scan_runs", "AC2", opt)
    must("ac2_full_scan", "AC2", test)

    # AC3 soft empty
    must("Soft probe", "AC3", opt)
    must("ac4_soft_empty", "AC3", test)

    # AC4 helpers + schema
    must("type_ir_union_cone_size", "AC4", dirty)
    must("type_ir_union_cone_nonempty", "AC4", dirty)
    must("schema-2556", "AC4", q)
    must("dirty-cone-cast-sites-scanned", "AC4", q)
    must("full-scan-runs", "AC4", q)
    must("dirty-cone-partial-runs", "AC4", q)

    # AC5 gate
    must("test_dead_coercion_dirty_cone_2556", "AC5", cmake)
    must("check_dead_coercion_dirty_cone_2556", "AC5", build)
    must("cmd_dead_coercion_dirty_cone_coverage", "AC5", build)
    must("ac5_source_schema", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2556 DCE dirty-cone scan limit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

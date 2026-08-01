#!/usr/bin/env python3
"""Issue #2519: LayoutStamp::operator== must include all 8 fields.

Contract:
  AC1 operator== compares shape_version + ir_soa_generation
  AC2 IR gen / shape mismatch via full equality
  AC3 production fence uses is_fully_fresh (not core-6)
  AC4 no production layout_core_equal for freshness
  AC5 schema-2519 + layout-stamp-equality-8-field + gate

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

    stamp = _read("src/core/layout_stamp.hh")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_layout_stamp_equality_8field_2519.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2519", "AC1", stamp)
    must("shape_version == o.shape_version", "AC1", stamp)
    must("ir_soa_generation == o.ir_soa_generation", "AC1", stamp)
    must("layout_core_equal", "AC1", stamp)
    must("is_fully_fresh", "AC1", stamp)
    must("is_shape_or_ir_unset", "AC1", stamp)
    must("ac1_equality_matrix", "AC1", test)

    # AC2
    must("ac2_ir_gen_equality", "AC2", test)
    must("is_fully_fresh", "AC2", fm)

    # AC3
    must("ac3_shape_version_equality", "AC3", test)
    must("Issue #2519", "AC3", fm)

    # AC4 — production fence must not call layout_core_equal
    if "layout_core_equal" in fm:
        fails.append("AC4: production fiber_mutation must not call layout_core_equal")
    must("layout_stamp_from_fiber_resume", "AC4", fm)
    must("ac4_no_6field_freshness", "AC4", test)

    # AC5
    must("kLayoutStampEqualitySchema", "AC5", stamp)
    must("layout-stamp-equality-8-field", "AC5", q)
    must("schema-2519", "AC5", q)
    must("test_layout_stamp_equality_8field_2519", "AC5", cmake)
    must("check_layout_stamp_equality_8field_2519", "AC5", build)
    must("cmd_layout_stamp_equality_8field_coverage", "AC5", build)
    must("ac5_query_schema", "AC5", test)

    # Retain prior schema
    must("kLayoutStampSchema = 2432", "retain", stamp)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2519 LayoutStamp 8-field equality — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

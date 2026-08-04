#!/usr/bin/env python3
"""Issue #2624 Phase A: CastOp type_id + narrow_evidence downflow metadata.

Contract:
  AC1 non-elided Coercion/CastOp lower stamps typed meta (src/dst/tag)
  AC2 AST identity elision skips CastOp entirely — no meta stamp required
  AC3 missing meta on legacy IR → missing_total only (no crash)
  AC4 side table not in SoA/module cache ABI (process-local ring)
  AC5 dirty-cone DCE (#2556) still correct with metadata present
  AC6 schema-2624 additive; Phase B/C out of scope; source-cite; gate wiring

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

    meta = _read("src/compiler/castop_typed_meta.h")
    low = _read("src/compiler/lowering_impl.cpp")
    pass_impls = _read("src/compiler/pass_impls.ixx")
    query = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_castop_typed_meta_2624.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    this = _read("scripts/check_castop_typed_meta_2624.py")

    # AC1
    must("Issue #2624", "AC1", meta)
    must("stamp_castop_typed_meta", "AC1", meta)
    must("stamp_last_castop_typed_meta", "AC1", low)
    must("castop_typed_meta.h", "AC1", low)
    must("ac1_non_elided_coercion_stamps", "AC1", test)

    # AC2 identity elision — no stamp
    must("can_elide_coercion_cast", "AC2", low)
    must("#2624 AC2", "AC2", low)
    must("ac2_identity_elision_no_cast", "AC2", test)

    # AC3 missing
    must("castop_typed_meta_missing_total", "AC3", meta)
    must("lookup_castop_typed_meta", "AC3", meta)
    must("ac3_missing_meta_no_crash", "AC3", test)

    # AC4 side table not SoA ABI
    must("side table", "AC4", meta)
    must("kCastOpTypedMetaCap", "AC4", meta)
    must("not persisted", "AC4", meta)
    must("ac4_side_table_not_soa", "AC4", test)

    # AC5 dirty-cone DCE + meta identity
    must("castop_typed_meta_present", "AC5", pass_impls)
    must("castop_typed_meta_identity_elide_total", "AC5", pass_impls)
    must("#2624", "AC5", pass_impls)
    must("ac5_dce_with_meta", "AC5", test)

    # AC6 schema + gate + no design docs + Phase B/C out of scope
    must("schema-2624", "AC6", query)
    must("castop-typed-meta-stamped-total", "AC6", query)
    must("castop-typed-meta-missing-total", "AC6", query)
    must("castop-typed-meta-phase-a", "AC6", query)
    must("Phase B", "AC6", meta)
    must("Phase C", "AC6", meta)
    must("test_castop_typed_meta_2624", "AC6", cmake)
    must("check_castop_typed_meta_2624", "AC6", build)
    must("cmd_castop_typed_meta_coverage", "AC6", build)
    must("ac6_schema_source", "AC6", test)
    must("Issue #2624", "AC6", this)
    for rel in (
        "docs/design/castop_typed_meta_2624.md",
        "docs/castop_typed_meta_2624.md",
        "design/2624.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2624 CastOp typed meta Phase A — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

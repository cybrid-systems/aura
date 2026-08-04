#!/usr/bin/env python3
"""Issue #2611: stamp mutation_id + narrow_evidence on elided CastOp deopt meta.

Contract:
  AC1 stamp_elided_cast_deopt_meta on Rule 6 / evidence elide + expose_last_deopt_meta
  AC2 no stamp without narrow_evidence (skipped_no_evidence / early return)
  AC3 soft empty cone path does not stamp (DeadCoercionPass empty-cone early-out)
  AC4 schema-2611 keys on query:dead-coercion-layered-stats + source-cite
  AC5 test + cmake + build.py gate; no docs/design md for #2611

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

    def must_absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    meta = _read("src/compiler/dce_elided_deopt_meta.h")
    pass_impls = _read("src/compiler/pass_impls.ixx")
    opt = _read("src/compiler/optimization_passes.ixx")
    coercion = _read("src/compiler/coercion_map.ixx")
    query = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_dce_elided_deopt_meta_2611.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2611", "AC1", meta)
    must("stamp_elided_cast_deopt_meta", "AC1", meta)
    must("expose_last_deopt_meta", "AC1", meta)
    must("lookup_elided_cast_deopt_meta", "AC1", meta)
    must("stamp_elided_cast_deopt_meta", "AC1", pass_impls)
    must("dce_elided_deopt_meta.h", "AC1", pass_impls)
    must("ac1_production_elide_expose", "AC1", test)

    # AC2
    must("narrow_evidence == 0", "AC2", meta)
    must("dce_deopt_meta_skipped_no_evidence", "AC2", meta)
    must("ac2_no_evidence_no_stamp", "AC2", test)

    # AC3
    must("Soft probe", "AC3", opt)  # empty-cone lineage #2556
    must("ac3_soft_empty_cone", "AC3", test)

    # AC4 schema + cite
    must("schema-2611", "AC4", query)
    must("deopt-meta-stamped-total", "AC4", query)
    must("deopt-meta-last-mid", "AC4", query)
    must("deopt-meta-last-evidence", "AC4", query)
    must("deopt-meta-wired", "AC4", query)
    must("note_dce_narrow_hits", "AC4", opt)
    must("dce_elided_deopt_meta.h", "AC4", coercion)
    must("ac4_schema_source_cite", "AC4", test)

    # AC5 gate + no design docs for 2611
    must("test_dce_elided_deopt_meta_2611", "AC5", cmake)
    must("check_dce_elided_deopt_meta_2611", "AC5", build)
    must("cmd_dce_elided_deopt_meta_coverage", "AC5", build)
    must("ac5_no_docs", "AC5", test)
    # No new design markdown referencing 2611
    for rel in (
        "docs/design/dce_elided_deopt_meta_2611.md",
        "docs/dce_elided_deopt_meta_2611.md",
        "design/2611.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2611 DCE elided CastOp deopt meta — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

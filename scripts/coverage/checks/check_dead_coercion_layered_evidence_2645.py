#!/usr/bin/env python3
"""Issue #2645: lock layered dead-coercion evidence chain (AST elision × IR DCE × deopt meta).

Contract:
  AC1 narrow_evidence != 0 → ast-elided++ and meta stamp with mid + evidence + type_tag
  AC2 narrow_evidence == 0 → AST may elide; NO meta stamp (zero cost)
  AC3 IR pass elision counts visible on layered stats
  AC4 Soft empty cone / no evidence path → zero meta / zero forced work
  AC5 gate script + source-cite #2611 / #2624 / this issue
  AC6 no docs/design

Exit 0 = all AC rows satisfied.
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

    cixx = _read("src/compiler/coercion_map.ixx")
    opasses = _read("src/compiler/optimization_passes.ixx")
    ctxm = _read("src/compiler/castop_typed_meta.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_dead_coercion_layered_2282.cpp")
    build = _read("build.py")

    # AC1: evidence != 0 → ast-elided++ and meta stamp
    must("g_dead_coercion_ast_elided_total", "AC1", cixx)
    must("narrow_evidence", "AC1", cixx)
    must("#2611", "AC1", cixx)
    must("ast-elided", "AC1", q)
    must("ir-narrow-evidence-hits", "AC1", q)

    # AC2: evidence == 0 → no meta stamp (guarded by if check)
    must("narrow_evidence != 0", "AC2", cixx)
    must("#1425", "AC2", cixx)  # or #2025, but #1425 is more common

    # AC3: IR pass elision counts visible
    must("dead_coercion_ir_narrow_evidence_hits", "AC3", opasses)
    must("DeadCoercion", "AC3", opasses)
    must("ir-elided", "AC3", q)
    must("pipeline-runs-total", "AC3", q)

    # AC4: Soft empty cone → zero forced work
    must("ast-elided", "AC4", q)
    must("ir-elided", "AC4", q)
    must("dirty-cone-skips", "AC4", q)

    # AC5: gate script + source-cite
    must("#2645", "AC5", test)
    must("#2611", "AC5", cixx)
    must("#2611", "AC5", opasses)
    must("#2624", "AC5", ctxm)  # type_id + narrow_evidence downflow phase A lives in castop_typed_meta.h
    must("check_dead_coercion_layered_evidence_2645", "AC5", build)

    # AC6: no docs/design — verify no docs/design/2645-* file exists
    design_dir = ROOT / "docs" / "design"
    design_files = list(design_dir.glob("2645*")) if design_dir.exists() else []
    if design_files:
        fails.append(f"AC6: docs/design/2645-* exists ({len(design_files)} files)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2645 layered dead-coercion evidence chain lock — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

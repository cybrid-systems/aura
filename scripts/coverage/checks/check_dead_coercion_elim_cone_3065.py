#!/usr/bin/env python3
"""Issue #3065: DeadCoercion elim / residual CastOp remirror into type cone.

After AST identity elision or IR DCE, Production/Full persist elim'd
nodes via force_dead_coercion_elim_into_cone (union into last type cone).
Residual CastOp under production takes the same dirty-column path.
Soft/quiet: no new permanent dirty bits.

Contract:
  AC1 Production remirror after AST/IR elim; remutate cone nonempty
  AC2 Soft/quiet: no permanent dirty bits
  AC3 schema-3065 + type_ir_union_cone metrics + lineage #2556/#3007
  AC4 extend test_dead_coercion_dirty_cone; linter; no docs/design/; no test_issue_3065

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    dirty = _read("src/compiler/dirty_propagation.ixx")
    cm = _read("src/compiler/coercion_map.ixx")
    opt = _read("src/compiler/optimization_passes.ixx")
    impl = _read("src/compiler/pass_impls.ixx")
    q = read_query_prims()
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3065", "AC1 dirty", dirty)
    must("force_dead_coercion_elim_into_cone", "AC1 helper", dirty)
    must("mirror_type_affected_to_cascade", "AC1 SSOT cite", dirty)
    must("Issue #3065", "AC1 apply", cm)
    must("force_dead_coercion_elim_into_cone", "AC1 apply call", cm)
    must("note_elim_ast", "AC1 IR collect", impl)
    must("flush_elim_cone", "AC1 IR flush", impl)
    must("ac3065_1_production_elim_reenters_cone", "AC1 test", t)

    # AC2
    must("Soft / quiet", "AC2 helper gate", dirty)
    must("persist_elim_cone", "AC2 apply gate", cm)
    must("ac3065_2_soft_no_permanent_bits", "AC2 test", t)

    # AC3
    must("schema-3065", "AC3 query", q)
    must("dead-coercion-elim-cone-force-total", "AC3 force key", q)
    must("dead-coercion-elim-cone-wired", "AC3 wired", q)
    must("schema-2556", "AC3 lineage 2556", q)
    must("type_ir_cone_union_size_sum", "AC3 union sample", dirty)
    must("ac3065_3_schema_and_union_metrics", "AC3 test", t)

    # AC4
    must("Issue #3065", "AC4 residual sweep", opt)
    must("force_residual_castop_blocks_into_cone", "AC4 residual helper", dirty)
    must("force_residual_castop_blocks_into_cone", "AC4 sweep call", opt)
    must("check_dead_coercion_elim_cone_3065", "AC4 build", build)
    must("cmd_dead_coercion_elim_cone_3065", "AC4 cmd", build)
    must("ac3065_4_residual_and_linter", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3065.cpp").is_file():
        fails.append("AC4: test_issue_3065.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3065*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3065 DeadCoercion elim cone remirror — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

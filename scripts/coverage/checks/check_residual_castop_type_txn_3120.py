#!/usr/bin/env python3
"""Issue #3120: residual CastOp remirror after type-txn wipe.

Production persist leftover CastOp source AST / containing blocks
across mirror_type_affected_to_cascade wipe; remirror after a
non-empty type cone. Soft / empty persist is observe-only.

Contract:
  AC1 type-changing mutate remirrors previously cone-skipped CastOp
  AC2 Soft persist / remirror no-ops (zero extra)
  AC3 reuse residual / type_cone counters; no schema-3120
  AC4 extend test_dead_coercion_dirty_cone; linter; no docs/design/; no test_issue_3120
  AC5 abort / map / desync / impact_ub force-full paths unchanged

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
    opt = _read("src/compiler/optimization_passes.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    tch = _read("src/compiler/type_checker.ixx")
    q = read_query_prims()
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")

    must("Issue #3120", "AC1 dirty", dirty)
    must("note_residual_castop_sites", "AC1 persist", dirty)
    must("remirror_persisted_residual_castops", "AC1 remirror", dirty)
    must("note_residual_castop_sites", "AC1 sweep persist", opt)
    must("remirror_persisted_residual_castops", "AC1 type txn", impl)
    must("ac3120_1_type_txn_remirrors_skipped_castop", "AC1 test", t)

    must("Soft / empty persist", "AC2 remirror gate", dirty)
    must("if (!residual_castop_persist_active())", "AC2 persist gate", dirty)
    must("ac3120_2_soft_zero_cost", "AC2 test", t)

    must("schema-3065", "AC3 reuse 3065", q)
    if "schema-3120" in q:
        fails.append("AC3: new query key schema-3120 (forbidden)")
    must("force_dead_coercion_elim_into_cone", "AC3 reuse force", dirty)
    must("ac3120_3_no_new_query_keys", "AC3 test", t)

    must("check_residual_castop_type_txn_3120", "AC4 build.py", build)
    must("ac3120_4_linter_no_invent", "AC4 test", t)
    must("remirror_persisted_residual_castops", "AC4 dirty-txn comment", tch)
    if (ROOT / "tests" / "compiler" / "test_issue_3120.cpp").is_file():
        fails.append("AC4: test_issue_3120.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3120-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    must("force_dead_coercion_elim_into_cone", "AC5 force helper kept", dirty)
    must("force_residual_castop_blocks_into_cone", "AC5 residual helper kept", dirty)
    must("mirror_type_affected_to_cascade", "AC5 type txn mirror kept", impl)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3120 residual CastOp type-txn remirror — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2904: FlatAST dirty → columnar bitmask + ImpactScope.

Contract:
  AC1 mark_dirty_upward columnar fixed-point; legacy BFS env-gated
  AC2 scan_dirty_columns / dirty_nodes_in_range column-only + counters
  AC3 MetadataColumnsSnapshot / restore restores dirty_
  AC4 mark_dirty_upward_masked ImpactScope cone
  AC5 query:dirty-columnar schema-2904 atomics
  AC6 cascades_avoided + sparse re-dirty path
  AC7 tests + build wire; no docs/design/*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ast_h = _read("src/core/ast.ixx")
    impl = _read("src/core/ast_impl.cpp")
    evalf = _read("src/compiler/evaluator_eval_flat.cpp")
    query = read_query_prims()
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/core/test_dirty_column_lock.cpp")
    build = _read("build.py")

    # AC1
    must("#2904", "AC1", impl)
    must("AURA_DIRTY_LEGACY_TREE_WALK", "AC1", impl)
    must("dirty_legacy_tree_walk_total", "AC1", impl)
    must("dirty_column_writes_total", "AC1", impl)
    must("dirty_upward_cascades_avoided_total", "AC1", impl)
    must("Columnar fixed-point", "AC1", impl)
    must("mark_dirty_columnar", "AC1", ast_h)

    # AC2
    must("scan_dirty_columns", "AC2", ast_h)
    must("scan_dirty_columns", "AC2", impl)
    must("dirty_scan_nodes_total", "AC2", impl)
    must("scan_dirty_columns", "AC2 post-mutate", evalf)

    # AC3
    must("MetadataColumnsSnapshot", "AC3", ast_h)
    must("restore_metadata_columns", "AC3", ast_h)
    must("dirty_", "AC3 restore", ast_h)

    # AC4
    must("mark_dirty_upward_masked", "AC4", ast_h)
    must("mark_dirty_upward_masked", "AC4", impl)
    must("admitted", "AC4 mask", impl)

    # AC5
    must("query:dirty-columnar", "AC5", query)
    must("schema-2904", "AC5", query)
    must("dirty-column-writes-total", "AC5", query)
    must("dirty-upward-cascades-avoided-total", "AC5", query)
    must("dirty-scan-nodes-total", "AC5", query)
    must("query:dirty-columnar", "AC5 obs", obs)

    # AC6
    must("dirty_upward_cascades_avoided_total", "AC6", impl)

    # AC7 tests + build
    must("ac2904_1_columnar_default_no_legacy_walk", "AC7", test)
    must("ac2904_2_scan_dirty_columns_only", "AC7", test)
    must("ac2904_3_rollback_restores_dirty", "AC7", test)
    must("ac2904_4_impact_scope_mask", "AC7", test)
    must("ac2904_5_query_and_atomics", "AC7", test)
    must("ac2904_6_sparse_early_exit", "AC7", test)
    must("ac2904_7_no_docs_design_source_cite", "AC7", test)
    must("check_dirty_columnar_2904", "AC7", build)
    must("cmd_dirty_columnar_2904", "AC7", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2904-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "core" / "test_issue_2904.cpp").is_file():
        fails.append("tests/core/test_issue_2904.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2904 FlatAST dirty columnar + ImpactScope — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

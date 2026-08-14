#!/usr/bin/env python3
"""Issue #3045: ADT exhaustiveness under-mark cone-force.

Production + variant add / arm delete → containing match site forced
into the dirty cone; hard reject if still non-exhaustive. Soft observe
only; Quiet (no ADT ancestor) zero extra.

Contract:
  AC1 Production / Full: ancestor force + dirty_propagation cone + Hard reject
  AC2 Soft under-mark → counter only
  AC3 Quiet empty / no ADT ancestor → zero extra
  AC4 schema-3045 + extend adt / mutate_type_gate / dirty-cone tests
  AC5 source cites dirty_propagation + evaluator_typecheck + mutate_type_gate
  AC6 this linter wired in build.py; no test_issue_3045.cpp; no docs/design/

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ev = _read("src/compiler/evaluator_typecheck.cpp")
    dp = _read("src/compiler/dirty_propagation.ixx")
    gate = _read("src/compiler/mutate_type_gate.hh")
    h = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = read_query_prims()
    test = _read("tests/compiler/test_adt_match_goal_table.cpp")
    mtg = _read("tests/compiler/test_mutate_type_gate.cpp")
    cone = _read("tests/compiler/test_type_dirty_cone_dep_graph.cpp")
    build = _read("build.py")

    # AC1
    must("#3045", "AC1 impl", impl)
    must("kAdtExhaustUndermarkConeIssue", "AC1", ixx)
    must("force_adt_exhaust_undermark_into_cone", "AC1", ixx)
    must("force_adt_exhaust_undermark_into_cone", "AC1", impl)
    must("force_adt_exhaust_undermark_from_match_nodes", "AC1", impl)
    must("collect_adt_ancestors_from_dirty", "AC1", impl)
    must("force_adt_exhaust_sites_into_cone", "AC1 dirty", dp)
    must("adt_exhaust_undermark_force_total", "AC1", impl)
    must("ac3045_1_undermark_force_cone", "AC1", test)

    # AC2
    must("adt_exhaust_soft_observe_total", "AC2", impl)
    must("production_defaults_active()", "AC2", impl)
    must("ac3045_2_soft_observe", "AC2", test)

    # AC3
    must("no ADT ancestor", "AC3", impl)
    must("match_sites.empty()", "AC3 dirty", dp)
    must("ac3045_3_quiet", "AC3", test)

    # AC4
    must("schema-3045", "AC4", q)
    must("adt-exhaust-undermark-force-total", "AC4", q)
    must("adt_exhaust_undermark_force_total", "AC4", h)
    must("adt_exhaust_undermark_force_total", "AC4 fields", fields)
    must("ac3045_4_schema", "AC4", test)
    must("#3045", "AC4 mutate_type_gate test", mtg)
    must("force_adt_exhaust_sites_into_cone", "AC4 dirty-cone test", cone)

    # AC5
    must("force_adt_exhaust_sites_into_cone", "AC5 dirty_propagation", dp)
    must("force_adt_exhaust_undermark_into_cone", "AC5 evaluator_typecheck", ev)
    must("mutate_type_gate", "AC5 evaluator_typecheck", ev)
    must("#3045", "AC5 mutate_type_gate", gate)
    must("under-mark cone-force", "AC5 mutate_type_gate", gate)
    must("ac3045_5_source_cites", "AC5", test)

    # AC6
    must("check_adt_exhaust_undermark_cone_3045", "AC6", build)
    must("cmd_adt_exhaust_undermark_cone_3045_coverage", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3045.cpp").is_file():
        fails.append("tests/compiler/test_issue_3045.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3045*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3045 ADT exhaustiveness under-mark cone-force")
    return 0


if __name__ == "__main__":
    sys.exit(main())

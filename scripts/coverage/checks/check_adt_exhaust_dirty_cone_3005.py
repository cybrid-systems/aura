#!/usr/bin/env python3
"""Issue #3005: ADT variant / pattern mutate → exhaustiveness dirty cone.

Production / Full hard-reject non-exhaustive or Dynamic-slide after
re-solve; Soft observe; Quiet when the goal was never dirty.

Contract:
  AC1 seed reverify roots into dirty cone + solve_delta touched/pending
  AC2 Production / Full no Dynamic slide; hard reject + authority drop
  AC3 Soft observe only
  AC4 Quiet empty → zero cost
  AC5 schema-3005 + lineage #2564/#2288/#2219/#2939
  AC6 extend test_adt_match_goal_table; linter; no docs/design/; no test_issue_3005

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
    h = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = read_query_prims()
    test = _read("tests/compiler/test_adt_match_goal_table.cpp")
    build = _read("build.py")

    # AC1
    must("#3005", "AC1", impl)
    must("kAdtExhaustDirtyConeIssue", "AC1", ixx)
    must("seed_adt_reverify_from_match_nodes", "AC1", ixx)
    must("seed_adt_reverify_from_match_nodes", "AC1", impl)
    must("note_adt_exhaust_dirty_type", "AC1", impl)
    must("adt_exhaust_cone_seed_total", "AC1", impl)
    must("pending_full_solve_roots_", "AC1 seed", impl)
    must("ac3005_1_cone_seed", "AC1", test)

    # AC2
    must("via_dynamic", "AC2", ixx)
    must("via_dynamic", "AC2", impl)
    must("via_dynamic", "AC2", ev)
    must("adt_exhaust_production_reject_total", "AC2", impl)
    must("adt_exhaust_dynamic_slide_prevented_total", "AC2", impl)
    must("last_type_export_authoritative_ = false", "AC2", impl)
    must("exhaustiveness unproven (Dynamic subject)", "AC2", ev)
    must("ac3005_2_production_no_dynamic", "AC2", test)

    # AC3
    must("adt_exhaust_soft_observe_total", "AC3", impl)
    must("production_defaults_active()", "AC3", impl)
    must("ac3005_3_soft_observe", "AC3", test)

    # AC4
    must("empty input → zero cost", "AC4", impl)
    must("ac3005_4_quiet_empty", "AC4", test)

    # AC5
    must("schema-3005", "AC5", q)
    must("adt-exhaust-cone-seed-total", "AC5", q)
    must("adt-exhaust-production-reject-total", "AC5", q)
    must("adt-exhaust-dynamic-slide-prevented-total", "AC5", q)
    must("schema-2564", "AC5 lineage", q)
    must("adt_exhaust_cone_seed_total", "AC5", h)
    must("adt_exhaust_cone_seed_total", "AC5 fields", fields)
    must("ac3005_5_schema_lineage", "AC5", test)

    # AC6
    must("check_adt_exhaust_dirty_cone_3005", "AC6", build)
    must("cmd_adt_exhaust_dirty_cone_3005_coverage", "AC6", build)
    must("ac3005_6_linter_no_design", "AC6", test)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3005*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_3005.cpp").is_file():
        fails.append("tests/compiler/test_issue_3005.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3005 ADT exhaustiveness dirty cone — Production no Dynamic slide")
    return 0


if __name__ == "__main__":
    sys.exit(main())

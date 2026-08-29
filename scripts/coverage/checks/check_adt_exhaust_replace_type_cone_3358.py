#!/usr/bin/env python3
"""Issue #3358: ADT exhaustiveness after ReplaceType / structural mutate.

Residual of #2223/#2264: exhaustiveness is not guaranteed in the dirty
cone when only a Match / Variant constructor is ReplaceType'd. Force the
enclosing parent (cone expansion of 1). production/Full: adt_ok=false
force-rollbacks via the unified authority table. Soft observes
adt_non_exhaustive_sites_total. No new query key.

Contract:
  AC1 Production/Full: parent cone + AdtNonExhaustive force-rollback
  AC2 Soft observe-only; no force-rollback for ADT
  AC3 reuse existing cone APIs + adt_non_exhaustive_sites_total
  AC4 extend type/occurrence/linear/audit tests; this linter after #3317

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

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
    dp = _read("src/compiler/dirty_propagation.ixx")
    sd = _read("src/compiler/service_dirty.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    goal = _read("tests/compiler/test_adt_match_goal_table.cpp")
    hard = _read("tests/compiler/test_adt_hard_gate_exhaustiveness.cpp")
    lin = _read("tests/compiler/test_linear_force_unified.cpp")
    audit = _read("tests/compiler/test_adt_exhaustiveness_audit.cpp")
    cone = _read("tests/compiler/test_type_dirty_cone_dep_graph.cpp")
    occ = _read("tests/compiler/test_occurrence_coercion_batch.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    build = _read("build.py")

    must("kAdtExhaustReplaceTypeConeIssue = 3358", "AC1 stamp", ixx)
    must("force_enclosing_match_parent_into_cone", "AC1 helper", ixx)
    must("force_enclosing_match_parent_into_cone", "AC1 impl", impl)
    must("expand_adt_enclosing_parent_into_cone", "AC1 dirty", dp)
    must("force_enclosing_match_parent_into_cone", "AC1 replace-type", mut)
    must("Issue #3358", "AC1 replace-type cite", mut)
    must("AdtNonExhaustive = 6", "AC1 enum", eixx)
    must("LinearForceAuthority::AdtNonExhaustive", "AC1 classify", etc)
    must("case LinearForceAuthority::AdtNonExhaustive", "AC1 switch", etc)
    must("ac3358_1_production_force_via_authority", "AC1 hard-gate test", hard)
    must("ac3358_1_replace_type_parent_cone", "AC1 cone test", goal)

    must("adt_non_exhaustive_sites_total", "AC2 Soft counter", aud)
    must("ac3358_2_soft_observe_only", "AC2 hard-gate test", hard)
    must("ac3358_3_soft_observe", "AC2 cone test", goal)

    must("force_adt_exhaust_sites_into_cone", "AC3 reuse cone", dp)
    must("expand_adt_enclosing_parent_into_cone", "AC3 undermark", impl)
    must("#3358", "AC3 service_dirty", sd)
    must("AdtNonExhaustive", "AC3 authority table", aud)
    must("ac3358_adt_authority", "AC3 linear test", lin)
    must("mutate-then-match", "AC3 audit fixture", audit)
    must("expand_adt_enclosing_parent_into_cone", "AC3 dirty-cone test", cone)
    must("run_test_adt_exhaustiveness_audit", "AC3 occurrence batch", occ)
    must("run_test_adt_hard_gate_exhaustiveness", "AC3 occurrence batch hard", occ)
    if "schema-3358" in q:
        fails.append("AC3: new schema-3358 query key")
    if "g_3358_" in impl or "g_3358_" in etc or "g_3358_" in dp:
        fails.append("AC3: new g_3358_* counter")

    must("check_adt_exhaust_replace_type_cone_3358", "AC4 build.py", build)
    must("ac3358_4_source_linter", "AC4 test", goal)
    prev = build.find("check_adt_exhaust_outermost_recheck_3317")
    ours = build.find("check_adt_exhaust_replace_type_cone_3358")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3317")
    if (ROOT / "tests" / "compiler" / "test_issue_3358.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3358.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3358.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3358.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3358-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3358 adt_exhaust_replace_type_cone:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3358 adt_exhaust_replace_type_cone: parent cone + AdtNonExhaustive force")
    return 0


if __name__ == "__main__":
    sys.exit(main())

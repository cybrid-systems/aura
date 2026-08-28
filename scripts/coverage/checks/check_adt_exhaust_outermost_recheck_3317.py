#!/usr/bin/env python3
"""Issue #3317: ADT exhaust complete recheck before TypeLinearCommitProof
on outermost success (close concurrent under-seed).

#3236 rechecks composite_txn_commit. Residual: concurrent variant add /
arm delete can under-seed match sites so outermost success stamps a green
TypeLinearCommitProof before a Production exhaust recheck. Reuses
force_adt_exhaust_* + #3005 reject / Soft observe counters. Soft observe;
quiet empty mutated-ADT set. No new query key.

Contract:
  AC1 Production + concurrent variant/arm mutate → non-exhaustive never
      reaches TypeLinearCommitProof / query:type authority
  AC2 Soft observe-only; no new hard reject under Soft
  AC3 reuse existing cone APIs + metrics (no new solve algorithm)
  AC4 extend test_adt_match_goal_table; this linter; no invent / docs

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
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    dp = _read("src/compiler/dirty_propagation.ixx")
    t = _read("tests/compiler/test_adt_match_goal_table.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    build = _read("build.py")

    must("kAdtExhaustOutermostRecheckIssue = 3317", "AC1 stamp", ixx)
    must("recheck_all_live_adt_exhaust_before_proof", "AC1 helper", ixx)
    must("recheck_all_live_adt_exhaust_before_proof", "AC1 impl", impl)
    must("recheck_all_live_adt_exhaust_before_proof", "AC1 persist", mb)
    must("Issue #3317", "AC1 persist cite", mb)
    must("force_reason=*/16", "AC1 reused force_reason", mb)
    must("clear_type_export_authority", "AC1 drop grant", mb)
    must("ac3317_1_production_no_green_nonexhaustive", "AC1 test", t)

    must("adt_exhaust_soft_observe_total", "AC2 soft", impl)
    must("ac3317_2_soft_observe", "AC2 test", t)

    must("force_adt_exhaust_undermark_into_cone", "AC3 #3045", impl)
    must("seed_adt_matches_for_dirty_types", "AC3 #3083", impl)
    must("check_match_exhaustiveness", "AC3 exhaust API", impl)
    must("adt_exhaust_production_reject_total", "AC3 reject metric", impl)
    must("force_adt_exhaust_sites_into_cone", "AC3 dirty cone", dp)
    must("Issue #3317", "AC3 typecheck cite", etc)
    must("ac3317_3_reuse_apis", "AC3 test", t)
    if "schema-3317" in q:
        fails.append("AC3: new schema-3317 query key")
    if "g_3317_" in impl or "g_3317_" in mb:
        fails.append("AC3: new g_3317_* counter")

    must("check_adt_exhaust_outermost_recheck_3317", "AC4 build.py", build)
    must("ac3317_4_source_linter", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3317.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3317.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3317.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3317.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3317-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3317 adt_exhaust_outermost_recheck:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3317 adt_exhaust_outermost_recheck: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

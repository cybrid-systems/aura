#!/usr/bin/env python3
"""Issue #3236: ADT match+arms cone + Production exhaustiveness before proof.

#3045/#3083 seed match sites; residual: arms stay outside type∪IR cone and
composite_txn_commit can stamp TypeLinearCommitProof without an
exhaustiveness recheck. Production/Full revalidate before proof; Soft
observe; quiet (no ADT) two size reads. Reuses #3045 reject / Soft
observe counters and force_reason solve (1). No new query key.

Contract:
  AC1 match node + arms into cone; Production recheck before proof
  AC2 Soft observe; quiet empty
  AC3 no regression #3045/#3083/#3228
  AC4 extend test_adt_match_goal_table; linter; no invent / docs/design

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
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    dp = _read("src/compiler/dirty_propagation.ixx")
    t = _read("tests/compiler/test_adt_match_goal_table.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    build = _read("build.py")

    must("kAdtExhaustCommitRecheckIssue = 3236", "AC1 stamp", ixx)
    must("Issue #3236", "AC1 impl", impl)
    must("flat.children(nid)", "AC1 arms", impl)
    must("Issue #3236", "AC1 commit", etc)
    must("check_match_exhaustiveness", "AC1 recheck", etc)
    must("force_reason=*/1", "AC1 reused solve", etc)
    must("ac3236_1_match_and_arms_into_cone", "AC1 test", t)

    must("adt_exhaust_soft_observe_total", "AC2 soft", etc)
    must("ac3236_2_soft_quiet", "AC2 test", t)

    must("force_adt_exhaust_undermark_into_cone", "AC3 #3045", impl)
    must("seed_adt_matches_for_dirty_types", "AC3 #3083", impl)
    must("force_residual_castop_undermark_into_cone", "AC3 #3228", dp)
    must("ac3236_3_lineage", "AC3 test", t)
    if "schema-3236" in q:
        fails.append("AC3: new schema-3236 query key")
    if "g_3236_" in impl or "g_3236_" in etc:
        fails.append("AC3: new g_3236_* counter")

    must("check_adt_exhaust_commit_recheck_3236", "AC4 build.py", build)
    must("ac3236_4_source_linter", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3236.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3236.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3236.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3236.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3236-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3236 adt_exhaust_commit_recheck:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3236 adt_exhaust_commit_recheck: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

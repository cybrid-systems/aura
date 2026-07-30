#!/usr/bin/env python3
"""Issue #2359: occurrence_goals + predicate_memo epoch health query surface.

  AC1: pure successive-query stability keys
  AC2: epoch advance prune / memo stale lag / selective clear
  AC3: no schema break on #2104/#2278/#2307/#2308
  AC4: wired sentinel + schema-2359
  AC5: tests + query registration + build gate

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    q = _read("src/compiler/evaluator_primitives_query.cpp")
    tci = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    test = _read("tests/compiler/test_memo_goal_epoch_health_2359.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("cache-epoch", "AC1", q)
    must("occurrence-goals-live", "AC1", q)
    must("predicate-memo-live", "AC1", q)
    must("predicate-memo-stale-vs-epoch", "AC1", q)
    must("memo-goal-epoch-delta", "AC1", q)
    must("ac1_stable_successive_queries", "AC1", test)

    # AC2
    must("occurrence_goals_stale_vs_epoch", "AC2", tci)
    must("predicate_memo_stale_vs_epoch", "AC2", tci)
    must("prune_occurrence_goals", "AC2", test)
    must("invalidate_predicate_memo_for_min_gen", "AC2", test)
    must("ac2_epoch_advance_prune_and_memo_lag", "AC2", test)

    # AC3
    must("schema-2278", "AC3", test)
    must("occurrence-goal-sole-authority-wired", "AC3", test)
    must("schema-2308", "AC3", test)
    must("ac3_lineage_no_schema_break", "AC3", test)
    # Query still exposes lineage (no break)
    must("schema-2278", "AC3", q)
    must("schema-2308", "AC3", q)
    must("occurrence-goal-sole-authority-wired", "AC3", q)

    # AC4
    must("memo-goal-epoch-health-wired", "AC4", q)
    must("schema-2359", "AC4", q)
    must("issue-2359", "AC4", q)
    must("ac4_wired_sentinel", "AC4", test)

    # AC5
    must("Issue #2359", "AC5", q)
    must("last_predicate_memo_live_", "AC5", impl)
    must("last_predicate_memo_stale_vs_epoch_", "AC5", impl)
    must("test_memo_goal_epoch_health_2359", "AC5", cmake)
    must("check_memo_goal_epoch_health_2359", "AC5", build)
    must("cmd_memo_goal_epoch_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2359 memo-goal epoch health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

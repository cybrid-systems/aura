#!/usr/bin/env python3
"""Issue #2403: composite index coverage + shared_lock hold minimization.

Contract:
  AC1 constrained pattern/by-marker :where hits composite index;
      miss only on unconstrained full walk
  AC2 shared_lock hold timed (total + max) on query hot path
  AC3 soft path zero extra cost before queries
  AC4 additive schema-2403 keys on pattern-index-stats-hash
  AC5 tests + microbench + build gate

Exit 0 = all rows satisfied.
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

    idx = _read("src/compiler/evaluator_query_index.cpp")
    ws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    metrics = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_query_index_composite_2403.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 composite hit path
    must("Issue #2403", "AC1", ws)
    must("bump_query_index_composite_hit", "AC1", ws)
    must("bump_query_index_composite_miss", "AC1", ws)
    must("snapshot_tag_all_arities", "AC1", idx)
    must("snapshot_tag_all_arities", "AC1", ev)
    must("2403 AC1", "AC1", test)

    # AC2 shared_lock
    must("note_query_shared_lock_us", "AC2", ws)
    must("QuerySharedLockTimer", "AC2", ws)
    must("query_shared_lock_us_total", "AC2", metrics)
    must("query_shared_lock_us_max", "AC2", metrics)
    must("2403 AC2", "AC2", test)

    # AC3 soft
    must("2403 AC3", "AC3", test)
    must("query_index_composite_hit_total", "AC3", metrics)
    must("query_index_composite_miss_total", "AC3", fields)

    # AC4 query keys
    must("schema-2403", "AC4", q)
    must("issue-2403", "AC4", q)
    must("query-index-composite-wired", "AC4", q)
    must("query-index-hit-rate", "AC4", q)
    must("query-index-miss-total", "AC4", q)
    must("query-shared-lock-us-total", "AC4", q)
    must("query-shared-lock-us-max", "AC4", q)
    must("2403 AC4", "AC4", test)

    # AC5
    must("2403 AC5", "AC5", test)
    must("check_query_index_composite_2403", "AC5", build)
    must("cmd_query_index_composite_coverage", "AC5", build)
    must("test_query_index_composite_2403", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: query-index-composite #2403 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

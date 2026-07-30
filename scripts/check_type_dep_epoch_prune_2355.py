#!/usr/bin/env python3
"""Issue #2355: type_dep_graph_ epoch prune + NodeId invalidation coverage.

  AC1: TypeDepEdge + prune_type_dep_graph_epoch on set_cache_epoch
  AC2: kTypeDepBucketCap + invalidate bounds growth
  AC3: same-epoch / empty-span no-ops
  AC4: schema-2355 additive on type-dep-partial-merge-stats
  AC5: tests + gate registration

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

    tc = _read("src/compiler/type_checker.ixx")
    tci = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_type_dep_epoch_prune_2355.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("TypeDepEdge", "AC1", tc)
    must("prune_type_dep_graph_epoch", "AC1", tc)
    must("set_cache_epoch", "AC1", tc)
    must("g_type_dep_graph_stale_drop_total", "AC1", tc)
    must("type_dep_graph_stale_drop_total", "AC1", met)
    must("ac1_epoch_prune", "AC1", test)
    must("Issue #2355", "AC1", tc)

    # AC2
    must("kTypeDepBucketCap", "AC2", tc)
    must("invalidate_type_dep_for_nodes", "AC2", tc)
    must("invalidate_type_dep_for_nodes", "AC2", tci)
    must("type_dep_graph_edge_count", "AC2", tc)
    must("ac2_session_bound", "AC2", test)

    # AC3
    must("min_epoch == 0", "AC3", tc)
    must("dirty.empty()", "AC3", tc)
    must("ac3_zero_cost_paths", "AC3", test)

    # AC4
    must("schema-2355", "AC4", q)
    must("issue-2355", "AC4", q)
    must("type-dep-stale-drop-total", "AC4", q)
    must("type-dep-size", "AC4", q)
    must("type-dep-epoch-wired", "AC4", q)
    must("schema-2320", "AC4", q)
    must("ac4_query_schema", "AC4", test)
    must("type_dep_graph_stale_drop_total", "AC4", fields)

    # AC5
    must("ac5_source_cite_and_invalidate", "AC5", test)
    must("test_type_dep_epoch_prune_2355", "AC5", cmake)
    must("check_type_dep_epoch_prune_2355", "AC5", build)
    must("cmd_type_dep_epoch_prune_coverage", "AC5", build)
    must("Issue #2355", "AC5", tci)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2355 type_dep epoch prune — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

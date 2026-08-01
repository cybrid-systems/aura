#!/usr/bin/env python3
"""Issue #2523: residual workspace_mtx contention stats + soft path.

Contract:
  AC1 source cites #2523 residual strategy
  AC2 disjoint regions avoid dual global exclusive (optimistic hits)
  AC3 global exclusive for try_acquire / policy-off
  AC4 mixed stress retained
  AC5 query:workspace-mtx-contention-stats + hold/wait/collision keys
  AC6 throughput microbench + gate wiring
  AC7 registered test

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

    boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_workspace_mtx_contention_2523.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2523", "AC1", boundary)
    must("#2523", "AC1", boundary)
    must("Soft path residual", "AC1", boundary)
    must("workspace_mtx_optimistic_hit_total", "AC1", boundary)
    must("query:workspace-mtx-contention-stats", "AC1", boundary)
    must("ac1_source_docs", "AC1", test)

    # AC2
    must("try_acquire_for_region", "AC2", boundary)
    must("workspace_mtx_optimistic_hit_total", "AC2", met)
    must("ac2_disjoint_not_dual_global", "AC2", test)

    # AC3
    must("GlobalExclusive", "AC3", boundary)
    must("workspace_global_exclusive_total", "AC3", boundary)
    must("ac3_global_and_fallback", "AC3", test)

    # AC4
    must("ac4_mixed_stress", "AC4", test)

    # AC5
    must("query:workspace-mtx-contention-stats", "AC5", q)
    must("query:workspace-mtx-contention-stats", "AC5", obs)
    must("workspace-mtx-hold-ns-p99", "AC5", q)
    must("workspace-mtx-optimistic-hit-total", "AC5", q)
    must("workspace-mtx-region-collision-total", "AC5", q)
    must("workspace-mtx-waiters-peak", "AC5", q)
    must("schema-2523", "AC5", q)
    must("workspace_mtx_optimistic_hit_total", "AC5", fields)
    must("workspace_mtx_region_collision_total", "AC5", fields)
    must("ac5_contention_query", "AC5", test)

    # AC6 + orch soft path
    must("try_acquire_for_region", "AC6", fiber)
    must("#2523", "AC6", fiber)
    must("ac6_throughput", "AC6", test)

    # AC7 gate
    must("test_workspace_mtx_contention_2523", "AC7", cmake)
    must("check_workspace_mtx_contention_2523", "AC7", build)
    must("cmd_workspace_mtx_contention_coverage", "AC7", build)

    # Retain #2121 lineage
    must("schema-2121", "retain", q)
    must("workspace_region_acquire_total", "retain", met)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2523 workspace_mtx residual contention — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

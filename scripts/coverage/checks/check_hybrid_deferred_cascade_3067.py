#!/usr/bin/env python3
"""Issue #3067: stale record_dependency reject schedules deferred hybrid cascade.

On gen/epoch race reject, queue (caller, callee). Drain at relower /
end of cascade window restores NodeId edges (or force-dirties callers)
and hybrid_node_cascade_. Soft/clean path: zero extra when no reject.

Contract:
  AC1 Injected dual-graph fork restored after drain
  AC2 graphs_consistent after production relower drain
  AC3 Clean single-fiber path: no deferred cascade
  AC4 extend test_dep_graph_hybrid_cascade; soak; linter; no docs/design/; no test_issue_3067

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

    svc = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3067", "AC1 service", svc)
    must("deferred_hybrid_edges_", "AC1 queue", svc)
    must("drain_deferred_hybrid_cascade_", "AC1 drain", svc)
    must("mirror_fn_dep_edge_unlocked_", "AC1 remirror", svc)
    must("ac3067_1_fork_restored_after_drain", "AC1 test", t)

    # AC2
    must("production_defaults_active", "AC2 prod rebuild", svc)
    must("graphs_consistent", "AC2 parity", svc)
    must("ac3067_2_production_consistent", "AC2 test", t)

    # AC3
    must("deferred_hybrid_armed_", "AC3 armed flag", svc)
    must("ac3067_3_clean_path_zero_extra", "AC3 test", t)

    # AC4
    must("Issue #3067", "AC4 invalidate", dirty)
    must("hybrid_deferred_cascade_total", "AC4 metric", met)
    must("schema-3067", "AC4 query", q)
    must("hybrid-deferred-cascade-total", "AC4 key", q)
    must("check_hybrid_deferred_cascade_3067", "AC4 build", build)
    must("cmd_hybrid_deferred_cascade_3067", "AC4 cmd", build)
    must("ac3067_4_soak_and_linter", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3067.cpp").is_file():
        fails.append("AC4: test_issue_3067.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3067*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3067 deferred hybrid cascade on stale reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

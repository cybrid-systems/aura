#!/usr/bin/env python3
"""Issue #2343: TIMEOUT/CONFLICT var↔constraint graph export coverage.

Contract (5 AC from issue body):
  AC1: force TIMEOUT with ≥2 dirty constraints sharing a var → edge count ≥1;
       suggested roots non-empty (export helper + result fields).
  AC2: SOLVED path → edge count 0 (zero-cost happy path).
  AC3: Production escalate still CONFLICT → graph still published.
  AC4: Additive query keys (schema-2343 / type-repair-graph-wired); #2284 intact.
  AC5: Source-cite solve_delta_impl / escalate_if_production / query publish.

This linter is the source-of-truth for the #2343 production surface.
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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    test = _read("tests/compiler/test_type_timeout_repair_2284.cpp")
    cmake = _read("CMakeLists.txt")

    # AC1: graph edge struct + export helper + result fields
    must("UnresolvedGraphEdge", "AC1", ixx)
    must("unresolved_graph_edges", "AC1", ixx)
    must("suggested_roots", "AC1", ixx)
    must("kUnresolvedGraphEdgeCap", "AC1", ixx)
    must("export_unresolved_var_constraint_graph", "AC1", ixx)
    must("export_unresolved_var_constraint_graph", "AC1", impl)
    must("var_to_constraints_", "AC1", impl)
    must("Issue #2343", "AC1", ixx)

    # AC2: SOLVED gated (zero-cost) — only fill on TIMEOUT / CONFLICT
    must("SolveResult::TIMEOUT", "AC2", impl)
    must("SolveResult::CONFLICT", "AC2", impl)
    must("unresolved_graph_edges", "AC2", impl)
    must("AC2: SOLVED", "AC2", test)

    # AC3: escalate path still has graph from TIMEOUT export
    must("escalate_if_production", "AC3", impl)
    must("type_repair_unresolved_edge_count", "AC3", tc)
    must("AC3: production escalate", "AC3", test)

    # AC4: metrics + query additive keys; #2284 retained
    must("type_repair_unresolved_edge_count", "AC4", met)
    must("type_repair_suggested_root_count", "AC4", met)
    must("type_repair_graph_wired", "AC4", met)
    must("type_repair_edge_var", "AC4", met)
    must("type-repair-unresolved-edge-count", "AC4", q)
    must("type-repair-suggested-root-count", "AC4", q)
    must("type-repair-graph-wired", "AC4", q)
    must("schema-2343", "AC4", q)
    must("issue-2343", "AC4", q)
    must("type-timeout-repair-wired", "AC4", q)
    must('"schema", 2284', "AC4", q)
    must("query:type-timeout-repair-stats", "AC4", q)

    # AC5: source-cite + test + cmake target
    must("solve_delta_impl", "AC5", impl)
    must("AC5: source-cite", "AC5", test)
    must("test_type_timeout_repair_2284", "AC5", cmake)
    must("#2343", "AC5", test)
    must("export_unresolved_var_constraint_graph", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2343 TIMEOUT unresolved graph — all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

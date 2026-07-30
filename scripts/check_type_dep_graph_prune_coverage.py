#!/usr/bin/env python3
"""
Linter for #2320 — prune stale NodeIds from type_dep_graph_ on cache_epoch
advance (bounded live entries). Closes the long-session cost-control
gap: type_dep_graph_ is append-only; long multi-round Agent sessions grow
vectors without bound; affected_nodes_for_type and partial merge cost
drift upward. After #2310-#2319 correctness fixes, the prune gate is
the cost-control piece.

Verifies the implementation is wired correctly:
  - observability_metrics.h: new per-CompilerMetrics counters
    (type_dep_graph_prune_total + type_dep_graph_entries_dropped +
    type_dep_graph_cap_evict_total) for prune observability
  - type_checker.ixx: prune_type_dep_graph(const FlatAST&) method
    + type_dep_graph_ field
  - type_checker_impl.cpp: prune call from infer_flat_partial entry
  - evaluator_primitives_query.cpp: query:type-dep-partial-merge-stats
    extended with new keys + schema-2320 / issue-2320
  - tests/compiler/test_type_dep_partial_merge_2283.cpp: extended
    with ac2320_* test functions + Issue #2320 cite

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_type_dep_graph_prune_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # observability_metrics.h: 3 new per-CompilerMetrics counters
        (
            ROOT / "src/compiler/observability_metrics.h",
            "type_dep_graph_prune_total{0}; // #2320",
            "observability_metrics.h: type_dep_graph_prune_total counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "type_dep_graph_entries_dropped{0}; // #2320",
            "observability_metrics.h: type_dep_graph_entries_dropped counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "type_dep_graph_cap_evict_total{0}; // #2320",
            "observability_metrics.h: type_dep_graph_cap_evict_total counter",
        ),
        # type_checker.ixx: prune method + type_dep_graph_ field
        (
            ROOT / "src/compiler/type_checker.ixx",
            "void prune_type_dep_graph",
            "type_checker.ixx: prune_type_dep_graph method present",
        ),
        (
            ROOT / "src/compiler/type_checker.ixx",
            "std::unordered_map<std::uint32_t, std::vector<aura::ast::NodeId>> type_dep_graph_",
            "type_checker.ixx: type_dep_graph_ field present",
        ),
        (ROOT / "src/compiler/type_checker.ixx", "n >= flat_size", "type_checker.ixx: prune checks NodeId range"),
        (
            ROOT / "src/compiler/type_checker.ixx",
            "flat.type_id(n) != tid",
            "type_checker.ixx: prune checks type_id match",
        ),
        (ROOT / "src/compiler/type_checker.ixx", "Issue #2320", "type_checker.ixx: cites Issue #2320"),
        # type_checker_impl.cpp: prune call from infer_flat_partial entry
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "prune_type_dep_graph(flat)",
            "type_checker_impl.cpp: prune call from infer_flat_partial entry",
        ),
        (ROOT / "src/compiler/type_checker_impl.cpp", "Issue #2320", "type_checker_impl.cpp: cites Issue #2320"),
        # evaluator_primitives_query.cpp: query keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "type-dep-graph-prune-total",
            "query primitive: type-dep-graph-prune-total key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "type-dep-graph-entries-dropped",
            "query primitive: type-dep-graph-entries-dropped key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "type-dep-graph-cap-evict-total",
            "query primitive: type-dep-graph-cap-evict-total key",
        ),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "schema-2320", "query primitive: schema-2320 sentinel"),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "issue-2320", "query primitive: issue-2320 sentinel"),
        # No regression of #2283 / #387 keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "type-dep-partial-merge-total",
            "query primitive: #2283 type-dep-partial-merge-total retained",
        ),
        # tests/compiler/test_type_dep_partial_merge_2283.cpp: ac2320_* test functions
        (
            ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp",
            "ac2320_prune_wiring",
            "test file: ac2320_prune_wiring function present",
        ),
        (
            ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp",
            "ac2320_query_keys_wired",
            "test file: ac2320_query_keys_wired function present",
        ),
        (
            ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp",
            "ac2320_soft_default_unchanged",
            "test file: ac2320_soft_default_unchanged function present",
        ),
        (
            ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp",
            "ac2320_prune_correctness",
            "test file: ac2320_prune_correctness function present",
        ),
        (
            ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp",
            "ac2320_source_cite_rows",
            "test file: ac2320_source_cite_rows function present",
        ),
        (ROOT / "tests/compiler/test_type_dep_partial_merge_2283.cpp", "Issue #2320", "test file: cites Issue #2320"),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_type_dep_graph_prune_coverage.py",
            "prune stale NodeIds from type_dep_graph",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2320 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

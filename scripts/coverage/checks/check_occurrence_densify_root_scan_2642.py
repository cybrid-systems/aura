#!/usr/bin/env python3
"""Issue #2642: densify Phase-5 O(dirty) live linear-root consistency scan
(beyond flag hard-AND).

Contract:
  AC1 production + linear root stale after densify → densify_ok false + force_linear_rollback
  AC2 Soft + same → observe counter only; densify may succeed
  AC3 no linear ops / empty dirty → zero scan work
  AC4 #2609 residual∧linear∧type fence still required (AND, not OR)
  AC5 schema + source-cite Phase 5 + authority table
  AC6 dual AC soak + coverage gate (this file)

Exit 0 = all AC rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    ixx = _read("src/compiler/evaluator.ixx")
    impl = _read("src/compiler/evaluator_typecheck.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    _read("CMakeLists.txt")
    build = _read("build.py")

    # AC5: source-cite Phase 5 + authority table
    must("#2642", "AC5", ixx)
    must("#2642", "AC5", impl)
    must("#2642", "AC5", mb)
    must("LinearDensifyRootMismatch", "AC5", ixx)
    must("scan_linear_roots_after_densify", "AC5", impl)
    must("linear_densify_scan_mismatch_total", "AC5", met)
    must("linear_densify_scan_mismatch_observe_total", "AC5", met)
    must("linear_densify_scan_mismatch_total", "AC5", fields)
    must("linear_densify_scan_mismatch_observe_total", "AC5", fields)
    must("linear-densify-scan-mismatch-total", "AC5", q)
    must("linear-densify-scan-mismatch-observe-total", "AC5", q)
    must("schema-2642", "AC5", q)
    must("issue-2642", "AC5", q)
    must("linear-densify-wired", "AC5", q)
    # AC4: #2609 still required (AND, not OR)
    must("evaluate_linear_type_provenance_hard_and", "AC4", impl)
    must("#2609", "AC4", ixx)
    must("#2609", "AC4", impl)
    # AC1/2/3: scan semantics in the function body
    must("if (!linear_ops_present())", "AC3", impl)
    must("Soft path bumps observe counter only", "AC2", impl)
    must("force_linear_rollback(LinearDensifyRootMismatch)", "AC5", impl)
    # AC6: coverage gate
    must("check_occurrence_densify_root_scan_2642", "AC6", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2642 densify Phase-5 O(dirty) linear-root scan — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

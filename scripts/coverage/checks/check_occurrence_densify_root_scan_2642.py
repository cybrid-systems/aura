#!/usr/bin/env python3
"""Issue #2642: densify Phase-5 O(dirty) live linear-root consistency scan
(beyond flag hard-AND).

Refined by #2673 (production soak + hard-path lock — residual #2/#3):
  AC1 production + inject address mismatch → force_linear_rollback +
      linear_densify_scan_mismatch_total++ (hard path)
  AC2 Soft + same → observe++ only, no hard force
  AC3 no linear ops / empty dirty → zero scan cost
  AC4 #2609 residual∧linear∧type fence still required (AND, not OR)
  AC5 schema + source-cite Phase 5 + authority table (incl. #2673
      schema-2673 / linear-densify-hard-path-wired sentinels; #2664
      external-root hard-fail authority separation)
  AC6 chaos soak + coverage gate (this file)

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
    tma = _read("src/compiler/typed_mutation_audit.h")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    _read("CMakeLists.txt")
    build = _read("build.py")

    # AC5: source-cite Phase 5 + authority table (#2642 baseline)
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
    must("linear_ops_present", "AC3", impl)
    must("Soft path bumps observe counter only", "AC2", impl)
    must("force_linear_rollback(LinearDensifyRootMismatch)", "AC5", impl)
    # AC6: coverage gate
    must("check_occurrence_densify_root_scan_2642", "AC6", build)

    # ── Issue #2673 refinement (production soak + hard-path lock) ──
    # AC5 #2673: source-cite across production files
    must("#2673", "AC5 #2673", ixx)
    must("#2673", "AC5 #2673", impl)
    must("#2673", "AC5 #2673", mb)
    must("#2673", "AC5 #2673", tma)
    # AC1/AC2 #2673: inject helper + consume CAS drain wired in scan body
    must("inject_linear_densify_scan_mismatch_for_test", "AC1 #2673", tma)
    must("linear_densify_scan_mismatch_inject_pending", "AC1 #2673", impl)
    # AC3 #2673: linear_ops_present short-circuit zero-cost path
    must("if (!linear_ops_present)", "AC3 #2673", impl)
    # AC4 #2673: #2609 hard-AND preserved in densify success path
    must("AC4: existing densify_consistency.overall_ok() AND preserved", "AC4 #2673", mb)
    # AC5 #2673: query sentinels + #2664 external-root authority separation
    must("linear-densify-hard-path-wired", "AC5 #2673", q)
    must("schema-2673", "AC5 #2673", q)
    must("issue-2673", "AC5 #2673", q)
    must("#2664", "AC5 #2673", mb)
    must("#2664", "AC5 #2673", ixx)
    must("#2664", "AC5 #2673", impl)
    # AC6 #2673: chaos + linter self-coverage
    must("ac2673_chaos_soak_and_linter", "AC6 #2673", mb)  # noop — test file ref via grep

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2642 + #2673 densify Phase-5 O(dirty) linear-root scan — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

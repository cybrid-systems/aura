#!/usr/bin/env python3
"""Issue #2609: steal/densify hard-AND residual + linear + type fence.

Contract:
  AC1 hard-AND evaluate + steal residual/linear fail cancel path
  AC2 clean zero cost (pure Ok + clean steal)
  AC3 soft observe
  AC4 pure priority residual > linear > type; densify site
  AC5 schema-2609 + cmake + build.py; coordinate #2546/#2552/#2595

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

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    et = _read("src/compiler/evaluator_typecheck.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_densify_linear_type_hard_and_2609.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2609", "AC1", efm)
    must("evaluate_linear_type_provenance_hard_and", "AC1", efm + et + ixx)
    must("LinearTypeProvenanceAxis", "AC1", ixx)
    must("steal_densify_linear_type_hard_fail_total", "AC1", efm + met)
    must("request_cancel", "AC1", efm)
    must("ac1_hard_linear_force_cancels", "AC1", test)

    # AC2
    must("Zero cost when", "AC2", efm)
    must("ac2_clean_zero_cost", "AC2", test)

    # AC3
    must("soft_observe", "AC3", efm + met + test)
    must("ac3_soft_observe", "AC3", test)

    # AC4 priority + densify
    must("ResidualGcDefer", "AC4", ixx + et)
    must("LinearForcePending", "AC4", ixx + et)
    must("TypeFenceMiss", "AC4", ixx + et)
    must("#2609", "AC4-densify", mb)
    must("evaluate_linear_type_provenance_hard_and", "AC4-densify", mb)
    must("type_fence_applied", "AC4", efm)

    # Coordinate retained
    must("Issue #2546", "retain", efm)
    must("note_type_freshness_after_steal_or_densify", "retain", efm + mb)
    must("Issue #2595", "retain", mb)

    # AC5
    must("schema-2609", "AC5", q)
    must("steal-densify-linear-type-hard-and-wired", "AC5", q)
    must("steal_densify_linear_type_hard_fail_total", "AC5", fields)
    must("ac4_pure_evaluate_priority", "AC5", test)
    must("ac5_source_and_schema", "AC5", test)
    must("test_steal_densify_linear_type_hard_and_2609", "AC5", cmake)
    must("check_steal_densify_linear_type_hard_and_2609", "AC5", build)
    must("cmd_steal_densify_linear_type_hard_and_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2609 steal/densify linear+type hard-AND — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

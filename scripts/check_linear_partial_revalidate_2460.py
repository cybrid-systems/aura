#!/usr/bin/env python3
"""Issue #2460: Phase-2 dirty OwnershipEnv re-sim during infer_flat_partial.

Contract:
  AC1 partial path elevates ownership fail (set_node_error + counters)
  AC2 Phase-1 #2357 path retained
  AC3 Soft Warning vs production/strict TypeError
  AC4 escape / post_mutation / #2108 retained
  AC5 schema-2460 + zero-cost empty + gate wiring

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

    tci = _read("src/compiler/type_checker_impl.cpp")
    tch = _read("src/compiler/type_checker.ixx")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_linear_partial_revalidate_2460.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("Issue #2460", "AC1", tci)
    must("last_partial_linear_revalidate_fail_", "AC1", tci)
    must("linear_partial_revalidate_fail_total", "AC1", tci)
    must("validate_ownership", "AC1", tci)
    must("set_node_error", "AC1", tci)
    must("discover_linear_bindings_in_subtree", "AC1", tci)
    must("AC1: partial ownership fail is first-class", "AC1", test)

    must("note_linear_synth_violation", "AC2", tci)
    must("AC2: Phase-1 synthesize path retained", "AC2", test)

    must("production_defaults_active", "AC3", tci)
    must("ErrorKind::Warning", "AC3", tci)
    must("ErrorKind::TypeError", "AC3", tci)
    must("AC3: Soft Warning vs production TypeError", "AC3", test)

    must("AC4: escape gate + #2108 retained", "AC4", test)
    must("post_mutation_invariant_check", "AC4", tci + tch)

    must("linear_partial_revalidate_total", "AC5", met)
    must("linear_partial_revalidate_fail_total", "AC5", met)
    must("linear_partial_revalidate_hard_fail_total", "AC5", met)
    must("linear_partial_revalidate_total", "AC5", fields)
    must("schema-2460", "AC5", q)
    must("linear-partial-revalidate-total", "AC5", q)
    must("schema-2357", "AC5", q)
    must("last_partial_linear_revalidate_fail", "AC5", tch)
    must("test_linear_partial_revalidate_2460", "gate", cmake)
    must("check_linear_partial_revalidate_2460", "gate", build)
    must("cmd_linear_partial_revalidate_coverage", "gate", build)
    must("AC5: empty dirty + schema-2460", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: linear partial revalidate #2460 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2558: coercion provenance completeness SLO coverage.

Contract:
  AC1 evaluate_coercion_provenance_slo + force pending under production
  AC2 Soft observe-only path
  AC3 Vacuous 10000 documented
  AC4 Boundary consume + fill-path evaluate; #2512 backstop comment
  AC5 query:coercion-provenance-health schema-2558 + test + cmake + gate

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

    pol = _read("src/compiler/coercion_provenance_policy.hh")
    cmap = _read("src/compiler/coercion_map.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_coercion_prov_slo_2558.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2558", "AC1", pol)
    must("evaluate_coercion_provenance_slo", "AC1", pol)
    must("g_coercion_prov_slo_force_full_pending", "AC1", pol)
    must("kCoercionProvSloBpDefault", "AC1", pol)
    must("ac1_production_slo_force", "AC1", test)

    # AC2
    must("g_coercion_prov_slo_observe_only_total", "AC2", pol)
    must("ac2_soft_observe", "AC2", test)

    # AC3
    must("vacuously", "AC3", pol)
    must("ac3_vacuous", "AC3", test)

    # AC4
    must("evaluate_coercion_provenance_slo", "AC4", cmap)
    must("consume_coercion_prov_slo_force_full", "AC4", bound)
    must("#2512", "AC4", pol)
    must("ac4_stamp_backstop", "AC4", test)

    # AC5
    must("query:coercion-provenance-health", "AC5", q)
    must("schema-2558", "AC5", q)
    must("force-full-pending", "AC5", q)
    must("test_coercion_prov_slo_2558", "AC5", cmake)
    must("check_coercion_prov_slo_2558", "AC5", build)
    must("cmd_coercion_prov_slo_coverage", "AC5", build)
    must("ac5_source_schema", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2558 coercion provenance SLO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

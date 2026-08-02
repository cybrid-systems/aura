#!/usr/bin/env python3
"""Issue #2562: dual-field (pred+mid) require-or-drop coverage.

Contract:
  AC1 dual-require drop after incomplete fill (production/Full/env)
  AC2 Soft #2317 insert path preserved; dual drop not armed by default
  AC3 complete dual zero drop; identity elision still first; stamp gate
  AC4 schema-2562 + keys on fidelity / layered / health queries
  AC5 completeness_bp / miss counters remain authority; test + cmake + gate

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
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_coercion_dual_require_2562.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2562", "AC1", pol)
    must("g_coercion_dual_require_drop_total", "AC1", pol)
    must("AURA_COERCION_DUAL_REQUIRE", "AC1", pol)
    must("coercion_dual_require_active", "AC1", cmap)
    must("g_coercion_dual_require_drop_total", "AC1", cmap)
    must("ac1_production_dual_drop", "AC1", test)

    # AC2
    must("Issue #2317", "AC2", cmap)
    must("g_coercion_sampled_insert_incomplete_total", "AC2", cmap)
    must("ac2_soft_insert_no_drop", "AC2", test)

    # AC3
    must("coercion_entry_dual_complete", "AC3", cmap)
    must("ac3_complete_and_elision", "AC3", test)

    # AC4
    must("schema-2562", "AC4", q)
    must("coercion-dual-require-drop-total", "AC4", q)
    must("coercion-dual-require-enabled", "AC4", q)
    must("query:type-incremental-fidelity-stats", "AC4", q)
    must("query:dead-coercion-layered-stats", "AC4", q)
    must("ac4_schema_source", "AC4", test)

    # AC5
    must("g_coercion_provenance_miss_total", "AC5", cmap)
    must("test_coercion_dual_require_2562", "AC5", cmake)
    must("check_coercion_dual_require_2562", "AC5", build)
    must("cmd_coercion_dual_require_coverage", "AC5", build)
    must("ac5_authority", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2562 dual-field require-or-drop — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

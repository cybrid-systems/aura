#!/usr/bin/env python3
"""Issue #2350: query:type-system-health single Agent score.

Contract:
  AC1 Score definition in type_system_health.hh
  AC2 force_reason priority timeout > pin > provenance > castop > ok
  AC3 Pure / additive (register_stats_impl; existing queries untouched)
  AC4 Keys health-bp / force-reason / schema-2350
  AC5 Tests + source-cite + catalog entry

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

    hh = _read("src/compiler/type_system_health.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_type_system_health_2350.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 score definition
    must("health_bp", "AC1", hh)
    must("coercion_provenance_completeness", "AC1", hh)
    must("timeout_reject_rate_bp", "AC1", hh)
    must("linear_pin_miss_rate_bp", "AC1", hh)
    must("layered_dce_efficiency_bp", "AC1", hh)
    must("compute_type_system_health", "AC1", hh)
    must("ac1_vacuous_healthy", "AC1", test)

    # AC2 force_reason
    must("timeout-reject", "AC2", hh)
    must("pin-miss", "AC2", hh)
    must("provenance-miss", "AC2", hh)
    must("castop-density", "AC2", hh)
    must("ac2_force_reason_priority", "AC2", test)

    # AC3 pure additive
    must("query:type-system-health", "AC3", q)
    must("register_stats_impl", "AC3", q)
    must("ac3_ac4_query_surface", "AC3", test)

    # AC4 keys
    must("health-bp", "AC4", q)
    must("health-budget-bp", "AC4", q)
    must("force-reason", "AC4", q)
    must("schema-2350", "AC4", q)
    must("type-system-health-wired", "AC4", q)
    must("component-provenance-completeness-bp", "AC4", q)

    # AC5
    must("Issue #2350", "AC5", q)
    must("query:type-system-health", "AC5", obs)
    must("test_type_system_health_2350", "AC5", cmake)
    must("check_type_system_health_2350", "AC5", build)
    must("cmd_type_system_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2350 type-system-health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

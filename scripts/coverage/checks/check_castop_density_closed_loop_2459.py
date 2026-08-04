#!/usr/bin/env python3
"""Issue #2459: production CastOp density closed-loop (streak + gate reject).

Contract:
  AC1 Soft: no gate reject by default
  AC2 production/HARD streak → gate reject
  AC3 under budget streak reset
  AC4 first over-budget force-JIT without reject
  AC5 schema-2459 + lineage + wiring

Exit 0 = all rows satisfied.
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

    pol = _read("src/compiler/castop_density_policy.hh")
    sd = _read("src/compiler/service_dirty.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_castop_density_closed_loop_2459.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("Issue #2459", "AC1", pol)
    must("apply_density_closed_loop", "AC1", pol)
    must("AC1: soft no gate reject", "AC1", test)

    must("AURA_CASTOP_DENSITY_STREAK_GATE", "AC2", pol)
    must("castop_density_gate_reject_total", "AC2", pol)
    must("gate_reject", "AC2", pol)
    must("AC2: gate reject after streak", "AC2", test)

    must("streak resets", "AC3", pol.lower() + pol)  # docstring
    must("AC3: streak reset to 0", "AC3", test)
    must("dens <= budget", "AC3", pol)

    must("AC4: force-JIT on first over-budget", "AC4", test)
    must("AC4: no gate reject on first fire", "AC4", test)
    must("production_defaults_active", "AC4", pol)

    must("schema-2459", "AC5", q)
    must("castop-density-streak", "AC5", q)
    must("castop-density-gate-reject-total", "AC5", q)
    must("castop-density-production-default-wired", "AC5", q)
    must("schema-2358", "AC5", q)
    must("schema-2319", "AC5", q)
    must("castop_density_streak", "AC5", met)
    must("castop_density_gate_reject_total", "AC5", fields)
    must("test_castop_density_closed_loop_2459", "gate", cmake)
    must("check_castop_density_closed_loop_2459", "gate", build)
    must("cmd_castop_density_closed_loop_coverage", "gate", build)
    must("#2459", "gate", sd)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: castop density closed-loop #2459 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

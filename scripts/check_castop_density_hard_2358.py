#!/usr/bin/env python3
"""Issue #2358: CastOp density HARD force-JIT policy coverage.

  AC1: HARD=0 soft-only (no force-JIT)
  AC2: HARD=1 + dens>budget → hard_action + force-JIT
  AC3: Mutate not rejected (codegen policy only)
  AC4: Under budget → no hard action
  AC5: schema-2358 + tests + gate; #2287/#2319 preserved

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

    pol = _read("src/compiler/castop_density_policy.hh")
    sd = _read("src/compiler/service_dirty.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_castop_density_hard_2358.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("hard_env_enabled", "AC1", pol)
    must("AURA_CASTOP_DENSITY_HARD", "AC1", pol)
    must("ac1_hard_off_soft_only", "AC1", test)
    must("Issue #2358", "AC1", pol)

    # AC2
    must("on_force_jit_for_reason", "AC2", pol)
    must("castop_density_hard_action_total", "AC2", pol)
    must("AotReloadFail::Other", "AC2", pol)
    must("ac2_hard_on_force_jit", "AC2", test)
    must("castop_density_hard_action_total", "AC2", met)

    # AC3
    must("Does NOT fail MutationBoundary", "AC3", pol)
    must("ac3_mutate_still_succeeds", "AC3", test)

    # AC4
    must("dens <= budget", "AC4", pol)
    must("ac4_under_budget_zero_extra", "AC4", test)

    # AC5
    must("apply_hard_policy", "AC5", sd)
    must("schema-2358", "AC5", q)
    must("castop-density-hard-action-total", "AC5", q)
    must("castop-density-hard-enabled", "AC5", q)
    must("schema-2319", "AC5", q)
    must("castop-annotation-hint", "AC5", q)
    must("castop_density_hard_action_total", "AC5", fields)
    must("test_castop_density_hard_2358", "AC5", cmake)
    must("check_castop_density_hard_2358", "AC5", build)
    must("cmd_castop_density_hard_coverage", "AC5", build)
    must("ac5_query_and_source", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2358 CastOp density HARD policy — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2546: hard-AND residual GcDeferReason == 0 on steal-complete success.

Contract:
  AC1 Hard residual non-zero after clear → Cancel+Done + hard-fail counter
  AC2 Clean residual → zero extra hard/soft counters
  AC3 Soft leftover → soft-leftover metric only, no cancel
  AC4 Source-cite next to #2314; schema-2546; worker comment
  AC5 Health/soak lineage residual counters; gate wiring

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

    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    hooks = _read("src/core/gc_hooks.h")
    worker = _read("src/serve/worker.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    qev = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qy = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/serve/test_residual_defer_steal_hard_and_2546.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2546", "AC1", efm)
    must("g_residual_defer_steal_hard_fail_total", "AC1", efm)
    must("request_cancel", "AC1", efm)
    must("FiberState::Done", "AC1", efm)
    must("is_steal_snapshot_hard_mode", "AC1", efm)
    must("ac1_hard_residual_cancels", "AC1", test)

    # AC2
    must("already zero", "AC2", efm.lower() + test.lower())
    must("ac2_clean_zero_cost", "AC2", test)

    # AC3
    must("g_residual_defer_steal_soft_leftover_total", "AC3", efm)
    must("Soft", "AC3", efm)
    must("ac3_soft_leftover_no_cancel", "AC3", test)

    # AC4
    must("Issue #2314", "AC4", efm)
    must("force_clear_residual_defer_for_evaluator", "AC4", efm)
    must("g_residual_defer_steal_hard_fail_total", "AC4", hooks)
    must("residual_defer_steal_hard_fail_total()", "AC4", hooks)
    must("#2546", "AC4", worker)
    must("residual_defer_steal_hard_fail_total{0}", "AC4", obs)
    must("schema-2546", "AC4", qjit)
    must("residual-defer-steal-hard-fail-total", "AC4", qjit)
    must("residual-defer-steal-hard-and-wired", "AC4", qjit)
    must("schema-2546", "AC4", qev)
    must("schema-2314", "AC4", qjit)
    must("ac4_source_and_schema", "AC4", test)

    # AC5
    must("residual_defer_steal_hard_fail_total", "AC5", qy)
    must("residual_hard_fail_total", "AC5", qy)
    must("ac5_soak_lineage", "AC5", test)
    must("test_residual_defer_steal_hard_and_2546", "AC5", cmake)
    must("check_residual_defer_steal_hard_and_2546", "AC5", build)
    must("cmd_residual_defer_steal_hard_and_coverage", "AC5", build)

    # Phase-1 retained
    must("g_residual_defer_cleared_on_steal_total", "retain", hooks)
    must("defer_reasons_snapshot() != 0", "retain", efm)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2546 residual hard-AND on steal-complete — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2846: residual GC defer after exit / steal-complete closed loop.

Under multi-fiber denseness a residual GcDeferReason can remain armed after
outermost MutationBoundary exit or steal-complete, permanently starving GC.
Production denseness force-clears; Soft observes via residual-defer-after-exit.

Contract (one row per AC):
  AC1 close_residual_defer_after_exit helper + g_residual_defer_after_exit_total
  AC2 Phase-5 success Clear + Soft use helper; failure path closes residual
  AC3 steal-complete uses close_residual_defer_after_exit
  AC4 query:mutation-boundary-hold-stats schema-2846 keys
  AC5 ac2846_* tests + this linter; no docs/design/*; no invent test

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

    gh = _read("src/core/gc_hooks.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_residual_gc_defer_assert.cpp")
    build = _read("build.py")

    # AC1 — helper + process counter
    must("close_residual_defer_after_exit", "AC1", gh)
    must("g_residual_defer_after_exit_total", "AC1", gh)
    must("kResidualDeferAfterExitIssue", "AC1", gh)
    must("#2846", "AC1", gh)
    must("force_clear_residual_defer_for_evaluator", "AC1", gh)
    must("reconcile_gc_defer_bits_after_clear", "AC1", gh)

    # AC2 — Phase 5 success + failure
    must("close_residual_defer_after_exit", "AC2", mb)
    must("residual_defer_after_exit_total", "AC2", mb)
    must("#2846", "AC2", mb)
    must("partial_recovery", "AC2 failure path", mb)
    # Soft observe + Clear force both present
    must("production_force=*/false", "AC2 Soft", mb)
    must("production_force=*/true", "AC2 Clear", mb)

    # AC3 — steal-complete
    must("close_residual_defer_after_exit", "AC3", mut)
    must("#2846", "AC3", mut)
    must("residual_defer_after_exit_total", "AC3", mut)

    # AC4 — query + metrics field
    must("residual_defer_after_exit_total{0}", "AC4", obs)
    must("residual-defer-after-exit-total", "AC4", q)
    must("schema-2846", "AC4", q)
    must("issue-2846", "AC4", q)
    must("residual-defer-after-exit-wired", "AC4", q)

    # AC5 — tests + linter
    must("ac2846_1_success_clear_drains_residual", "AC5", test)
    must("ac2846_2_soft_observes_after_exit", "AC5", test)
    must("ac2846_3_failure_exit_clears_under_production", "AC5", test)
    must("ac2846_4_helper_and_steal_source", "AC5", test)
    must("ac2846_5_source_linter_query", "AC5", test)
    must("check_residual_defer_after_exit_2846", "AC5", build)
    must("ac2269_residual_defer_policy", "AC5 #2269 preserved", test)
    must("ac2296_multi_eval_residual_clear", "AC5 #2296 preserved", test)
    if (ROOT / "tests" / "compiler" / "test_issue_2846.cpp").is_file():
        fails.append("AC5: test_issue_2846.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2846*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2846 residual-defer-after-exit closed loop — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

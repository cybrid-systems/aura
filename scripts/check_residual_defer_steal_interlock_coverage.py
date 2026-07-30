#!/usr/bin/env python3
"""
Linter for #2314 — residual GcDeferReason clear must be atomic w.r.t.
steal-complete (no orphan window). Closes the long-session leak where
outermost Guard Phase-5 residual clear (#2269 Clear policy) and
steal-complete orphan panic clear (#2203) — running as separate entries —
could re-arm or leave orphan GcDeferReason bits accumulating across long
AI sessions.

Shared helper force_clear_residual_defer_for_evaluator (gc_hooks.h) is
called from steal-complete (evaluator_fiber_mutation.cpp
aura_evaluator_on_steal_complete — AC1.2 #2314 interlock). Guard Phase 5
(evaluator_mutation_boundary.cpp) preserves inline calls to satisfy the
#2296 contract rows check. Both paths perform the same essential
operations; the helper exists for steal-side reuse only. Idempotent
(atomic + CAS-based — calling twice does not double-bump counters).

Verifies the implementation is wired correctly:
  - gc_hooks.h exposes shared helper
    force_clear_residual_defer_for_evaluator(void*) and
    ResidualClearResult struct
  - gc_hooks.h process-wide counter
    g_residual_defer_cleared_on_steal_total + accessor
  - evaluator_fiber_mutation.cpp aura_evaluator_on_steal_complete calls
    shared helper when defer_reasons_snapshot() != 0 (AC1.2)
  - evaluator_mutation_boundary.cpp Guard Phase 5 Clear policy has
    INLINE calls (force_clear_all_gc_defer_for_evaluator + hold release +
    reconcile — #2296 contract requires these symbols in this file
    directly, not via helper)
  - observability_metrics.h per-CompilerMetrics
    residual_defer_cleared_on_steal_total counter
  - evaluator_primitives_obs_jit.cpp query:gc-defer-reason-stats exposes
    schema-2314 / issue-2314 / counter keys
  - evaluator_primitives_obs_eval.cpp query:mutation-boundary-hold-stats
    exposes same lineage keys
  - tests/serve/test_steal_complete_gc_defer_2203.cpp cites Issue #2314

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_residual_defer_steal_interlock_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # gc_hooks.h shared helper + counter
        (ROOT / "src/core/gc_hooks.h", "force_clear_residual_defer_for_evaluator", "gc_hooks.h has shared helper"),
        (ROOT / "src/core/gc_hooks.h", "ResidualClearResult", "gc_hooks.h has ResidualClearResult struct"),
        (
            ROOT / "src/core/gc_hooks.h",
            "g_residual_defer_cleared_on_steal_total",
            "gc_hooks.h has process-wide counter",
        ),
        (ROOT / "src/core/gc_hooks.h", "residual_defer_cleared_on_steal_total()", "gc_hooks.h has counter accessor"),
        (ROOT / "src/core/gc_hooks.h", "Idempotent", "gc_hooks.h documents idempotency"),
        (ROOT / "src/core/gc_hooks.h", "Issue #2314", "gc_hooks.h cites 2314"),
        # evaluator_fiber_mutation.cpp steal-complete calls helper
        (
            ROOT / "src/compiler/evaluator_fiber_mutation.cpp",
            "force_clear_residual_defer_for_evaluator",
            "steal-complete calls shared helper",
        ),
        (
            ROOT / "src/compiler/evaluator_fiber_mutation.cpp",
            "defer_reasons_snapshot() != 0",
            "steal-complete guards on snapshot non-zero",
        ),
        (ROOT / "src/compiler/evaluator_fiber_mutation.cpp", "Issue #2314", "evaluator_fiber_mutation.cpp cites 2314"),
        # evaluator_mutation_boundary.cpp Guard Phase 5 uses inline calls
        # (NOT the helper — #2296 contract rows check requires these
        # symbols directly in evaluator_mutation_boundary.cpp).
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "force_clear_all_gc_defer_for_evaluator",
            "Guard Phase 5 has inline force_clear_all_gc_defer_for_evaluator",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "release_mutation_hold_defer",
            "Guard Phase 5 has inline release_mutation_hold_defer",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "reconcile_gc_defer_bits_after_clear",
            "Guard Phase 5 has inline reconcile_gc_defer_bits_after_clear",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "Issue #2314",
            "evaluator_mutation_boundary.cpp cites 2314",
        ),
        # observability_metrics.h per-CompilerMetrics counter
        (
            ROOT / "src/compiler/observability_metrics.h",
            "residual_defer_cleared_on_steal_total{0}; // #2314",
            "observability_metrics.h has counter",
        ),
        # query:gc-defer-reason-stats
        (
            ROOT / "src/compiler/evaluator_primitives_obs_jit.cpp",
            "residual-defer-cleared-on-steal-total",
            "query:gc-defer-reason-stats counter key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_jit.cpp",
            "residual-defer-steal-interlock-wired",
            "query:gc-defer-reason-stats wired sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_jit.cpp",
            "schema-2314",
            "query:gc-defer-reason-stats schema-2314",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_jit.cpp",
            "issue-2314",
            "query:gc-defer-reason-stats issue-2314",
        ),
        # query:mutation-boundary-hold-stats
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "residual-defer-cleared-on-steal-total",
            "query:mutation-boundary-hold-stats counter key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "residual-defer-steal-interlock-wired",
            "query:mutation-boundary-hold-stats wired sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "schema-2314",
            "query:mutation-boundary-hold-stats schema-2314",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "issue-2314",
            "query:mutation-boundary-hold-stats issue-2314",
        ),
        # test file
        (ROOT / "tests/serve/test_steal_complete_gc_defer_2203.cpp", "Issue #2314", "test file cites 2314"),
        (
            ROOT / "tests/serve/test_steal_complete_gc_defer_2203.cpp",
            "ac2314_residual_interlock",
            "test file has #2314 AC1 function",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_residual_defer_steal_interlock_coverage.py",
            "residual GcDeferReason clear must be atomic",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2314 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

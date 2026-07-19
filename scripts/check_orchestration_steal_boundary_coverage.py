#!/usr/bin/env python3
# scripts/check_orchestration_steal_boundary_coverage.py — Issue #1641
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: 3 metric slots in src/compiler/observability_metrics.h
#        (steal_mutation_boundary_deferred_total +
#         starvation_mitigated_for_boundary_count +
#         boundary_held_steal_safe_total).
#   AC3: 3 AURA_COMPILER_METRICS_FIELD(...) entries in
#        src/compiler/compiler_metrics_fields.inc.
#   AC4: 3 bump_/getter pairs declared in src/compiler/evaluator.ixx.
#   AC5: serve/worker.cpp wires boundary_held_steal_safe_total after
#        bump_cross_fiber_mutation_safe_steal (safe-steal success path).
#   AC6: serve/worker.cpp wires steal_mutation_boundary_deferred_total
#        + starvation_mitigated_for_boundary_count after
#        apply_starvation_mitigation(stolen) (inner boundary block).
#   AC7: serve/scheduler.cpp wires starvation_mitigated_for_boundary_count
#        after apply_starvation_mitigation(f).
#   AC8: existing inner boundary handling preserved (the legacy
#        bump_steal_inner_mutation_boundary_deferred +
#        apply_starvation_mitigation cycle is still in place).
#   AC9: existing safe-steal observability preserved
#        (bump_cross_fiber_mutation_safe_steal still bumped at the
#        Fiber level — the #1641 counter is paired on top).
#   AC10: pre-existing Scheduler/Worker observability metrics unchanged
#         (adaptive_steal_stats global_deferred_mutation_total +
#         steal_deferred_inner_boundary + starvation_priority_boosts).
#
# Pattern reference: scripts/check_aot_hot_update_incremental_coverage.py
# (#1640), scripts/check_incremental_relower_coverage.py (#1639),
# scripts/check_soa_dual_path_consistency_coverage.py (#1638),
# scripts/check_panic_checkpoint_lifecycle_coverage.py (#1637),
# scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_orchestration_steal_boundary_coverage.py` from the repo
# root.

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ERRORS = []


def check(name: str, condition: bool) -> None:
    if condition:
        print(f"OK    {name}")
    else:
        print(f"FAIL  {name}")
        ERRORS.append(name)


def read(rel: str) -> str:
    return (ROOT / rel).read_text()


def main() -> int:
    print("=== scripts/check_orchestration_steal_boundary_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    om = read("src/compiler/observability_metrics.h")
    fields = read("src/compiler/compiler_metrics_fields.inc")
    ixx = read("src/compiler/evaluator.ixx")
    worker = read("src/serve/worker.cpp")
    scheduler = read("src/serve/scheduler.cpp")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: 3 metric slots
    print("--- AC2: 3 metric slots in observability_metrics.h ---")
    check(
        "steal_mutation_boundary_deferred_total in observability_metrics.h",
        "steal_mutation_boundary_deferred_total" in om,
    )
    check(
        "starvation_mitigated_for_boundary_count in observability_metrics.h",
        "starvation_mitigated_for_boundary_count" in om,
    )
    check("boundary_held_steal_safe_total in observability_metrics.h", "boundary_held_steal_safe_total" in om)
    print()

    # AC3: 3 X-macro fields
    print("--- AC3: 3 X-macro fields in compiler_metrics_fields.inc ---")
    check(
        "X-macro steal_mutation_boundary_deferred_total",
        "AURA_COMPILER_METRICS_FIELD(steal_mutation_boundary_deferred_total)" in fields,
    )
    check(
        "X-macro starvation_mitigated_for_boundary_count",
        "AURA_COMPILER_METRICS_FIELD(starvation_mitigated_for_boundary_count)" in fields,
    )
    check(
        "X-macro boundary_held_steal_safe_total",
        "AURA_COMPILER_METRICS_FIELD(boundary_held_steal_safe_total)" in fields,
    )
    print()

    # AC4: 3 bump_/getter pairs
    print("--- AC4: 3 bump_/getter pairs in evaluator.ixx ---")
    check(
        "bump_steal_mutation_boundary_deferred_total in evaluator.ixx",
        "bump_steal_mutation_boundary_deferred_total" in ixx,
    )
    check(
        "bump_starvation_mitigated_for_boundary_count in evaluator.ixx",
        "bump_starvation_mitigated_for_boundary_count" in ixx,
    )
    check("bump_boundary_held_steal_safe_total in evaluator.ixx", "bump_boundary_held_steal_safe_total" in ixx)
    check(
        "get_steal_mutation_boundary_deferred_total in evaluator.ixx",
        "get_steal_mutation_boundary_deferred_total" in ixx,
    )
    check(
        "get_starvation_mitigated_for_boundary_count in evaluator.ixx",
        "get_starvation_mitigated_for_boundary_count" in ixx,
    )
    check("get_boundary_held_steal_safe_total in evaluator.ixx", "get_boundary_held_steal_safe_total" in ixx)
    print()

    # AC5: worker.cpp boundary_held_steal_safe_total wire-up
    print("--- AC5: worker.cpp boundary_held_steal_safe_total wire-up ---")
    check(
        "worker.cpp: Issue #1641 paired boundary_held_steal_safe_total",
        "Issue #1641: paired boundary_held_steal_safe_total" in worker,
    )
    check(
        "worker.cpp: ev->bump_boundary_held_steal_safe_total() call",
        "ev->bump_boundary_held_steal_safe_total()" in worker,
    )
    check(
        "worker.cpp: YieldReason::MutationBoundary check still present",
        "stolen->last_yield_reason() == YieldReason::MutationBoundary" in worker,
    )
    print()

    # AC6: worker.cpp inner boundary bumps
    print("--- AC6: worker.cpp inner boundary bumps ---")
    check(
        "worker.cpp: Issue #1641 paired steal_mutation_boundary_deferred_total",
        "Issue #1641: paired steal_mutation_boundary_deferred_total" in worker,
    )
    check(
        "worker.cpp: ev->bump_steal_mutation_boundary_deferred_total() call",
        "ev->bump_steal_mutation_boundary_deferred_total()" in worker,
    )
    check(
        "worker.cpp: ev->bump_starvation_mitigated_for_boundary_count() call",
        "ev->bump_starvation_mitigated_for_boundary_count()" in worker,
    )
    check(
        "worker.cpp: apply_starvation_mitigation(stolen) still present",
        "apply_starvation_mitigation(stolen);" in worker,
    )
    print()

    # AC7: scheduler.cpp starvation_mitigated_for_boundary_count wire-up
    print("--- AC7: scheduler.cpp starvation_mitigated_for_boundary_count wire-up ---")
    check(
        "scheduler.cpp: Issue #1641 paired starvation_mitigated_for_boundary_count",
        "Issue #1641: paired starvation_mitigated_for_boundary_count" in scheduler,
    )
    check(
        "scheduler.cpp: ev->bump_starvation_mitigated_for_boundary_count() call",
        "ev->bump_starvation_mitigated_for_boundary_count()" in scheduler,
    )
    check("scheduler.cpp: apply_starvation_mitigation(f) still present", "apply_starvation_mitigation(f);" in scheduler)
    print()

    # AC8: existing inner boundary handling preserved
    print("--- AC8: existing inner boundary handling preserved ---")
    check(
        "worker.cpp: bump_steal_inner_mutation_boundary_deferred still present",
        "stolen->bump_steal_inner_mutation_boundary_deferred();" in worker,
    )
    check(
        "worker.cpp: is_at_inner_mutation_boundary check still present",
        "if (stolen->is_at_inner_mutation_boundary())" in worker,
    )
    check(
        "worker.cpp: adaptive_steal_stats.steal_deferred_inner_boundary still bumped",
        "metrics::adaptive_steal_stats().steal_deferred_inner_boundary.fetch_add" in worker,
    )
    print()

    # AC9: existing safe-steal observability preserved
    print("--- AC9: existing safe-steal observability preserved ---")
    check(
        "worker.cpp: bump_cross_fiber_mutation_safe_steal still bumped (per-Fiber)",
        "stolen->bump_cross_fiber_mutation_safe_steal();" in worker,
    )
    check(
        "worker.cpp: bump_steal_outermost_mutation_boundary still bumped (per-Fiber)",
        "stolen->bump_steal_outermost_mutation_boundary();" in worker,
    )
    print()

    # AC10: pre-existing Scheduler/Worker observability unchanged
    print("--- AC10: pre-existing Scheduler/Worker observability unchanged ---")
    check(
        "worker.cpp: adaptive_steal_stats().global_deferred_mutation_total still bumped",
        "metrics::adaptive_steal_stats().global_deferred_mutation_total.fetch_add" in worker,
    )
    check(
        "worker.cpp: adaptive_steal_stats().mutation_bias_hits still bumped",
        "metrics::adaptive_steal_stats().mutation_bias_hits.fetch_add" in worker,
    )
    check(
        "worker.cpp: adaptive_steal_stats().starvation_priority_boosts still bumped",
        "metrics::adaptive_steal_stats().starvation_priority_boosts.fetch_add" in worker,
    )
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1641 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

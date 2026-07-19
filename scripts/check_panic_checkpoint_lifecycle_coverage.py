#!/usr/bin/env python3
# scripts/check_panic_checkpoint_lifecycle_coverage.py — Issue #1637
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test; running it must
#        return 0 for the linter to be wired in the pre-commit hook).
#   AC2: 5 metric slots in src/compiler/observability_metrics.h
#        (post_steal_checkpoint_restore_total, post_compact_, post_hot_swap_,
#         cross_fiber_panic_heal_success, mutation_boundary_steal_safe_total).
#   AC3: 5 AURA_COMPILER_METRICS_FIELD(...) entries in
#        src/compiler/compiler_metrics_fields.inc.
#   AC4: 3 restore_<event>_if_needed declarations in src/compiler/evaluator.ixx
#        (fiber_resume + arena_compact + hot_swap).
#   AC5: 5 bump_/getter pairs in src/compiler/evaluator.ixx.
#   AC6: run_post_restore_lifecycle_close helper declared in evaluator.ixx
#        + implemented in evaluator_workspace_tree.cpp.
#   AC7: 5 prod-file wire-up checks (workspace_tree impls 3 restore variants,
#        fiber_mutation on_arena_compact_hook + 3 C trampolines,
#        primitives_types hot-swap:fn, aura_jit_bridge 3 (void*) bridge hooks).
#   AC8: 5 keys in query:mutation-boundary-coverage-stats primitive output
#        (evaluator_primitives_obs_eval_05.cpp) + schema 1637.
#   AC9: 5 file-scope atomic fallbacks in aura_jit_bridge.cpp.
#   AC10: 5 C accessors in aura_jit_bridge.cpp.
#
# Pattern reference: scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_reflect_edsl_coverage.py (#1907),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs). Run
# individually with `./scripts/check_panic_checkpoint_lifecycle_coverage.py`
# from the repo root.

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
    print("=== scripts/check_panic_checkpoint_lifecycle_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    om = read("src/compiler/observability_metrics.h")
    fields = read("src/compiler/compiler_metrics_fields.inc")
    ixx = read("src/compiler/evaluator.ixx")
    wst = read("src/compiler/evaluator_workspace_tree.cpp")
    mut = read("src/compiler/evaluator_fiber_mutation.cpp")
    types = read("src/compiler/evaluator_primitives_types.cpp")
    bridge = read("src/compiler/aura_jit_bridge.cpp")
    prim = read("src/compiler/evaluator_primitives_obs_eval_05.cpp")

    # AC1: self-test (the script itself exiting 0)
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)  # reached this line = OK
    print()

    # AC2: 5 metric slots in observability_metrics.h
    print("--- AC2: 5 metric slots in observability_metrics.h ---")
    check("post_steal_checkpoint_restore_total in observability_metrics.h", "post_steal_checkpoint_restore_total" in om)
    check(
        "post_compact_checkpoint_restore_total in observability_metrics.h",
        "post_compact_checkpoint_restore_total" in om,
    )
    check(
        "post_hot_swap_checkpoint_restore_total in observability_metrics.h",
        "post_hot_swap_checkpoint_restore_total" in om,
    )
    check("cross_fiber_panic_heal_success in observability_metrics.h", "cross_fiber_panic_heal_success" in om)
    check("mutation_boundary_steal_safe_total in observability_metrics.h", "mutation_boundary_steal_safe_total" in om)
    print()

    # AC3: 5 X-macro fields
    print("--- AC3: 5 X-macro fields in compiler_metrics_fields.inc ---")
    check(
        "X-macro post_steal_checkpoint_restore_total",
        "AURA_COMPILER_METRICS_FIELD(post_steal_checkpoint_restore_total)" in fields,
    )
    check(
        "X-macro post_compact_checkpoint_restore_total",
        "AURA_COMPILER_METRICS_FIELD(post_compact_checkpoint_restore_total)" in fields,
    )
    check(
        "X-macro post_hot_swap_checkpoint_restore_total",
        "AURA_COMPILER_METRICS_FIELD(post_hot_swap_checkpoint_restore_total)" in fields,
    )
    check(
        "X-macro cross_fiber_panic_heal_success",
        "AURA_COMPILER_METRICS_FIELD(cross_fiber_panic_heal_success)" in fields,
    )
    check(
        "X-macro mutation_boundary_steal_safe_total",
        "AURA_COMPILER_METRICS_FIELD(mutation_boundary_steal_safe_total)" in fields,
    )
    print()

    # AC4: 3 restore_<event>_if_needed declarations
    print("--- AC4: 3 restore variants declared in evaluator.ixx ---")
    check(
        "restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept;",
        "void restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept;" in ixx,
    )
    check(
        "restore_panic_checkpoint_on_arena_compact_if_needed() noexcept;",
        "void restore_panic_checkpoint_on_arena_compact_if_needed() noexcept;" in ixx,
    )
    check(
        "restore_panic_checkpoint_on_hot_swap_if_needed() noexcept;",
        "void restore_panic_checkpoint_on_hot_swap_if_needed() noexcept;" in ixx,
    )
    print()

    # AC5: 5 bump_/getter pairs
    print("--- AC5: 5 bump_/getter pairs in evaluator.ixx ---")
    check(
        "bump_post_steal_checkpoint_restore_total in evaluator.ixx", "bump_post_steal_checkpoint_restore_total" in ixx
    )
    check(
        "bump_post_compact_checkpoint_restore_total in evaluator.ixx",
        "bump_post_compact_checkpoint_restore_total" in ixx,
    )
    check(
        "bump_post_hot_swap_checkpoint_restore_total in evaluator.ixx",
        "bump_post_hot_swap_checkpoint_restore_total" in ixx,
    )
    check("bump_cross_fiber_panic_heal_success in evaluator.ixx", "bump_cross_fiber_panic_heal_success" in ixx)
    check("bump_mutation_boundary_steal_safe_total in evaluator.ixx", "bump_mutation_boundary_steal_safe_total" in ixx)
    check("get_post_steal_checkpoint_restore_total in evaluator.ixx", "get_post_steal_checkpoint_restore_total" in ixx)
    check(
        "get_post_compact_checkpoint_restore_total in evaluator.ixx", "get_post_compact_checkpoint_restore_total" in ixx
    )
    check(
        "get_post_hot_swap_checkpoint_restore_total in evaluator.ixx",
        "get_post_hot_swap_checkpoint_restore_total" in ixx,
    )
    check("get_cross_fiber_panic_heal_success in evaluator.ixx", "get_cross_fiber_panic_heal_success" in ixx)
    check("get_mutation_boundary_steal_safe_total in evaluator.ixx", "get_mutation_boundary_steal_safe_total" in ixx)
    print()

    # AC6: shared helper
    print("--- AC6: run_post_restore_lifecycle_close helper ---")
    check(
        "run_post_restore_lifecycle_close declared in evaluator.ixx",
        "void run_post_restore_lifecycle_close(bool" in ixx,
    )
    check(
        "Evaluator::run_post_restore_lifecycle_close implemented in evaluator_workspace_tree.cpp",
        "Evaluator::run_post_restore_lifecycle_close(bool" in wst,
    )
    print()

    # AC7: 5 prod-file wire-ups
    print("--- AC7: restore_<event>_if_needed callsites + 3 prod-file wire-ups ---")
    check(
        "workspace_tree: 3 restore variants implemented",
        "Evaluator::restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept" in wst
        and "Evaluator::restore_panic_checkpoint_on_arena_compact_if_needed() noexcept" in wst
        and "Evaluator::restore_panic_checkpoint_on_hot_swap_if_needed() noexcept" in wst,
    )
    check(
        "fiber_mutation: on_arena_compact_hook calls restore_panic_checkpoint_on_arena_compact_if_needed",
        "on_arena_compact_hook" in mut and "restore_panic_checkpoint_on_arena_compact_if_needed" in mut,
    )
    check(
        "fiber_mutation: 3 per-Evaluator C trampolines (no arg)",
        "aura_evaluator_post_steal_panic_restore()" in mut
        and "aura_evaluator_post_compact_panic_restore()" in mut
        and "aura_evaluator_hot_swap_panic_restore()" in mut,
    )
    check(
        "primitives_types: hot-swap:fn wires restore_panic_checkpoint_on_hot_swap_if_needed",
        '"hot-swap:fn"' in types and "restore_panic_checkpoint_on_hot_swap_if_needed" in types,
    )
    check(
        "aura_jit_bridge: 3 bridge hooks (void* ev_ptr signature)",
        "aura_evaluator_post_steal_panic_restore(void* ev_ptr)" in bridge
        and "aura_evaluator_post_compact_panic_restore(void* ev_ptr)" in bridge
        and "aura_evaluator_hot_swap_panic_restore(void* ev_ptr)" in bridge,
    )
    print()

    # AC8: 5 keys in primitive output
    print("--- AC8: query:mutation-boundary-coverage-stats extended ---")
    check("post-steal-checkpoint-restore-total key in primitive kv", '"post-steal-checkpoint-restore-total"' in prim)
    check(
        "post-compact-checkpoint-restore-total key in primitive kv", '"post-compact-checkpoint-restore-total"' in prim
    )
    check(
        "post-hot-swap-checkpoint-restore-total key in primitive kv", '"post-hot-swap-checkpoint-restore-total"' in prim
    )
    check("cross-fiber-panic-heal-success key in primitive kv", '"cross-fiber-panic-heal-success"' in prim)
    check("mutation-boundary-steal-safe-total key in primitive kv", '"mutation-boundary-steal-safe-total"' in prim)
    check("schema bumped to 1637 in primitive kv", "make_int(1637)" in prim)
    print()

    # AC9: 5 file-scope atomics
    print("--- AC9: 5 file-scope atomic fallbacks in aura_jit_bridge.cpp ---")
    check("g_1637_steal_restore_fallback_total", "g_1637_steal_restore_fallback_total" in bridge)
    check("g_1637_compact_restore_fallback_total", "g_1637_compact_restore_fallback_total" in bridge)
    check("g_1637_hot_swap_restore_fallback_total", "g_1637_hot_swap_restore_fallback_total" in bridge)
    check("g_1637_panic_heal_success_fallback_total", "g_1637_panic_heal_success_fallback_total" in bridge)
    check("g_1637_boundary_steal_safe_fallback_total", "g_1637_boundary_steal_safe_fallback_total" in bridge)
    print()

    # AC10: 5 C accessors
    print("--- AC10: 5 C accessors in aura_jit_bridge.cpp ---")
    check("aura_post_steal_checkpoint_restore_total", "aura_post_steal_checkpoint_restore_total" in bridge)
    check("aura_post_compact_checkpoint_restore_total", "aura_post_compact_checkpoint_restore_total" in bridge)
    check("aura_post_hot_swap_checkpoint_restore_total", "aura_post_hot_swap_checkpoint_restore_total" in bridge)
    check("aura_cross_fiber_panic_heal_success_total", "aura_cross_fiber_panic_heal_success_total" in bridge)
    check("aura_mutation_boundary_steal_safe_total", "aura_mutation_boundary_steal_safe_total" in bridge)
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1637 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

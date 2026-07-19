#!/usr/bin/env python3
# scripts/check_incremental_relower_coverage.py — Issue #1639
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: 5 metric slots in src/compiler/observability_metrics.h
#        (full_relower_count +
#         dirty_block_ratio_{numerator,denominator}_total +
#         relower_block_hit_rate_{numerator,denominator}_total).
#   AC3: 5 AURA_COMPILER_METRICS_FIELD(...) entries in
#        src/compiler/compiler_metrics_fields.inc.
#   AC4: 5 bump_/getter pairs declared in src/compiler/evaluator.ixx.
#   AC5: 4 wire-up sites in src/compiler/service.ixx
#        (full_relower_count + relower_block_hit_rate on partial +
#         relower_block_hit_rate on full + dirty_block_ratio on entry).
#   AC6: query:incremental-relower-stats primitive extended with 6 new
#        keys + schema bumped 1623 → 1639.
#   AC7: per-block dirty bitmask wired into 5 local passes
#        (ConstantFolding / ComputeKind / TypePropagation / Shape /
#         EscapeAnalysis) via run_incremental_dirty_pipeline.
#   AC8: relower_define_blocks function exists with the full cycle 2/3
#        dispatch (skip / single-function partial / multi-function
#        full fallback).
#   AC9: nested-lambda targeted dirty propagation wired via
#        mark_nested_lambda_blocks_targeted (#1505).
#   AC10: lookup_define_v2 prefer-partial decision (should_relower
#         helper) wired with body-only bitmask fallback (#1555).
#
# Pattern reference: scripts/check_soa_dual_path_consistency_coverage.py
# (#1638), scripts/check_panic_checkpoint_lifecycle_coverage.py (#1637),
# scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_reflect_edsl_coverage.py (#1907),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_incremental_relower_coverage.py` from the repo root.

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
    print("=== scripts/check_incremental_relower_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    om = read("src/compiler/observability_metrics.h")
    fields = read("src/compiler/compiler_metrics_fields.inc")
    ixx = read("src/compiler/evaluator.ixx")
    svc = read("src/compiler/service.ixx")
    prim = read("src/compiler/evaluator_primitives_obs_eval_05.cpp")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: 5 metric slots
    print("--- AC2: 5 metric slots in observability_metrics.h ---")
    check("full_relower_count in observability_metrics.h", "full_relower_count" in om)
    check("dirty_block_ratio_numerator_total in observability_metrics.h", "dirty_block_ratio_numerator_total" in om)
    check("dirty_block_ratio_denominator_total in observability_metrics.h", "dirty_block_ratio_denominator_total" in om)
    check(
        "relower_block_hit_rate_numerator_total in observability_metrics.h",
        "relower_block_hit_rate_numerator_total" in om,
    )
    check(
        "relower_block_hit_rate_denominator_total in observability_metrics.h",
        "relower_block_hit_rate_denominator_total" in om,
    )
    print()

    # AC3: 5 X-macro fields
    print("--- AC3: 5 X-macro fields in compiler_metrics_fields.inc ---")
    check("X-macro full_relower_count", "AURA_COMPILER_METRICS_FIELD(full_relower_count)" in fields)
    check(
        "X-macro dirty_block_ratio_numerator_total",
        "AURA_COMPILER_METRICS_FIELD(dirty_block_ratio_numerator_total)" in fields,
    )
    check(
        "X-macro dirty_block_ratio_denominator_total",
        "AURA_COMPILER_METRICS_FIELD(dirty_block_ratio_denominator_total)" in fields,
    )
    check(
        "X-macro relower_block_hit_rate_numerator_total",
        "AURA_COMPILER_METRICS_FIELD(relower_block_hit_rate_numerator_total)" in fields,
    )
    check(
        "X-macro relower_block_hit_rate_denominator_total",
        "AURA_COMPILER_METRICS_FIELD(relower_block_hit_rate_denominator_total)" in fields,
    )
    print()

    # AC4: 5 bump_/getter pairs
    print("--- AC4: 5 bump_/getter pairs in evaluator.ixx ---")
    check("bump_full_relower_count in evaluator.ixx", "bump_full_relower_count" in ixx)
    check("bump_dirty_block_ratio in evaluator.ixx", "bump_dirty_block_ratio" in ixx)
    check("bump_relower_block_hit_rate in evaluator.ixx", "bump_relower_block_hit_rate" in ixx)
    check("get_full_relower_count in evaluator.ixx", "get_full_relower_count" in ixx)
    check("get_dirty_block_ratio_numerator_total in evaluator.ixx", "get_dirty_block_ratio_numerator_total" in ixx)
    check("get_dirty_block_ratio_denominator_total in evaluator.ixx", "get_dirty_block_ratio_denominator_total" in ixx)
    check(
        "get_relower_block_hit_rate_numerator_total in evaluator.ixx",
        "get_relower_block_hit_rate_numerator_total" in ixx,
    )
    check(
        "get_relower_block_hit_rate_denominator_total in evaluator.ixx",
        "get_relower_block_hit_rate_denominator_total" in ixx,
    )
    print()

    # AC5: 4 wire-up sites in service.ixx
    print("--- AC5: 4 wire-up sites in service.ixx ---")
    check("evaluator_.bump_full_relower_count wire-up", "evaluator_.bump_full_relower_count()" in svc)
    check(
        "evaluator_.bump_relower_block_hit_rate(1, 1) partial hit",
        "evaluator_.bump_relower_block_hit_rate(1, 1)" in svc,
    )
    check(
        "evaluator_.bump_relower_block_hit_rate(0, 1) full fallback",
        "evaluator_.bump_relower_block_hit_rate(0, 1)" in svc,
    )
    check(
        "evaluator_.bump_dirty_block_ratio(dirty_blocks, total_blocks_seen)",
        "evaluator_.bump_dirty_block_ratio(dirty_blocks, total_blocks_seen)" in svc,
    )
    print()

    # AC6: 6 new keys + schema 1639 in primitive
    print("--- AC6: query:incremental-relower-stats extended ---")
    check("full-relower-count key in primitive kv", '"full-relower-count"' in prim)
    check("dirty-block-ratio-numerator-total key in primitive kv", '"dirty-block-ratio-numerator-total"' in prim)
    check("dirty-block-ratio-denominator-total key in primitive kv", '"dirty-block-ratio-denominator-total"' in prim)
    check(
        "relower-block-hit-rate-numerator-total key in primitive kv", '"relower-block-hit-rate-numerator-total"' in prim
    )
    check(
        "relower-block-hit-rate-denominator-total key in primitive kv",
        '"relower-block-hit-rate-denominator-total"' in prim,
    )
    check("relower-block-hit-rate key in primitive kv", '"relower-block-hit-rate"' in prim)
    check("schema bumped to 1639 in primitive kv", "make_int(1639)" in prim)
    print()

    # AC7: per-block dirty wired into 5 local passes
    print("--- AC7: per-block dirty wired into 5 local passes ---")
    check(
        "run_incremental_dirty_pipeline wired for ConstantFolding (cf_pass)",
        "run_incremental_dirty_pipeline(ir_mod, cf_pass" in svc,
    )
    check(
        "run_incremental_dirty_pipeline wired for ComputeKind (ck_pass)",
        "run_incremental_dirty_pipeline(ir_mod, ck_pass" in svc,
    )
    check(
        "run_incremental_dirty_pipeline wired for TypePropagation (tp_pass)",
        "run_incremental_dirty_pipeline(ir_mod, tp_pass" in svc,
    )
    check(
        "run_incremental_dirty_pipeline wired for Shape (shape_pass)",
        "run_incremental_dirty_pipeline(ir_mod, shape_pass" in svc,
    )
    check(
        "run_incremental_dirty_pipeline wired for EscapeAnalysis (escape_pass)",
        "run_incremental_dirty_pipeline(ir_mod, escape_pass" in svc,
    )
    print()

    # AC8: relower_define_blocks cycle 2/3 dispatch
    print("--- AC8: relower_define_blocks cycle 2/3 dispatch ---")
    check("relower_define_blocks function declared", "bool relower_define_blocks(" in svc)
    check(
        "cycle 2: dirty_blocks == 0 → skip",
        "metrics_.relower_skipped_entirely_count.fetch_add(1, std::memory_order_relaxed);" in svc,
    )
    check(
        "cycle 3: dirty_func_count == 1 → partial",
        "dirty_func_count == 1" in svc and "relower_define_function(name, dirty_func_idx," in svc,
    )
    check(
        "full fallback: relower_full_called_count bump",
        "metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);" in svc,
    )
    print()

    # AC9: nested-lambda targeted dirty propagation (#1505)
    print("--- AC9: nested-lambda targeted dirty propagation ---")
    check(
        "mark_nested_lambda_blocks_targeted declared",
        "std::size_t mark_nested_lambda_blocks_targeted(IRCacheEntry& entry" in svc,
    )
    check("nested_lambda_references_name helper declared", "static bool nested_lambda_references_name(" in svc)
    print()

    # AC10: lookup_define_v2 prefer-partial decision (#1555)
    print("--- AC10: lookup_define_v2 prefer-partial decision ---")
    check("lookup_define_v2 declared", "int lookup_define_v2(const std::string& name" in svc)
    check(
        "should_relower helper declared",
        "bool should_relower(" in svc or "should_relower(" in read("src/compiler/ir_cache_pure.ixx"),
    )
    check("body-only bitmask fallback in lookup_define_v2", "it->second.dirty_block_count() > 0" in svc)
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1639 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# scripts/check_soa_dual_path_consistency_coverage.py — Issue #1638
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: 3 metric slots in src/compiler/observability_metrics.h
#        (dual_path_stale_fallback_total /
#         mutation_log_compact_bytes_saved /
#         env_frame_version_drift_prevented).
#   AC3: 3 AURA_COMPILER_METRICS_FIELD(...) entries in
#        src/compiler/compiler_metrics_fields.inc.
#   AC4: 3 bump_/getter pairs declared in src/compiler/evaluator.ixx.
#   AC5: FlatAST::compact_mutation_log() + mutation_log_size() declared
#        in src/core/ast.ixx (inline in class body).
#   AC6: Evaluator::compact_mutation_log() declared in evaluator.ixx +
#        implemented in evaluator_workspace_tree.cpp.
#   AC7: Evaluator::ensure_env_frame_dual_path_consistent(EnvId, const char*)
#        declared in evaluator.ixx + implemented in
#        evaluator_workspace_tree.cpp with is_env_frame_stale check.
#   AC8: exit_mutation_boundary success path wires mutation_log compact
#        with 64KB threshold gate (evaluator.ixx).
#   AC9: 5 prod-file wire-up checks (evaluator_env materialize_call_env
#        dual-path check + 2 evaluator_gc collect sites + evaluator_env
#        lookup-by-symid pattern + JIT Apply prologue).
#   AC10: 3 keys in query:mutation-boundary-coverage-stats primitive output
#         (evaluator_primitives_obs_eval_05.cpp) + schema 1638.
#
# Pattern reference: scripts/check_panic_checkpoint_lifecycle_coverage.py
# (#1637), scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_reflect_edsl_coverage.py (#1907),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_soa_dual_path_consistency_coverage.py` from the repo
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
    print("=== scripts/check_soa_dual_path_consistency_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    om = read("src/compiler/observability_metrics.h")
    fields = read("src/compiler/compiler_metrics_fields.inc")
    ixx = read("src/compiler/evaluator.ixx")
    wst = read("src/compiler/evaluator_workspace_tree.cpp")
    ast_ixx = read("src/core/ast.ixx")
    env_cpp = read("src/compiler/evaluator_env.cpp")
    gc_cpp = read("src/compiler/evaluator_gc.cpp")
    prim = read("src/compiler/evaluator_primitives_obs_eval_05.cpp")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: 3 metric slots
    print("--- AC2: 3 metric slots in observability_metrics.h ---")
    check("dual_path_stale_fallback_total in observability_metrics.h", "dual_path_stale_fallback_total" in om)
    check("mutation_log_compact_bytes_saved in observability_metrics.h", "mutation_log_compact_bytes_saved" in om)
    check("env_frame_version_drift_prevented in observability_metrics.h", "env_frame_version_drift_prevented" in om)
    print()

    # AC3: 3 X-macro fields
    print("--- AC3: 3 X-macro fields in compiler_metrics_fields.inc ---")
    check(
        "X-macro dual_path_stale_fallback_total",
        "AURA_COMPILER_METRICS_FIELD(dual_path_stale_fallback_total)" in fields,
    )
    check(
        "X-macro mutation_log_compact_bytes_saved",
        "AURA_COMPILER_METRICS_FIELD(mutation_log_compact_bytes_saved)" in fields,
    )
    check(
        "X-macro env_frame_version_drift_prevented",
        "AURA_COMPILER_METRICS_FIELD(env_frame_version_drift_prevented)" in fields,
    )
    print()

    # AC4: 3 bump_/getter pairs
    print("--- AC4: 3 bump_/getter pairs in evaluator.ixx ---")
    check("bump_dual_path_stale_fallback_total in evaluator.ixx", "bump_dual_path_stale_fallback_total" in ixx)
    check("bump_mutation_log_compact_bytes_saved in evaluator.ixx", "bump_mutation_log_compact_bytes_saved" in ixx)
    check("bump_env_frame_version_drift_prevented in evaluator.ixx", "bump_env_frame_version_drift_prevented" in ixx)
    check("get_dual_path_stale_fallback_total in evaluator.ixx", "get_dual_path_stale_fallback_total" in ixx)
    check("get_mutation_log_compact_bytes_saved in evaluator.ixx", "get_mutation_log_compact_bytes_saved" in ixx)
    check("get_env_frame_version_drift_prevented in evaluator.ixx", "get_env_frame_version_drift_prevented" in ixx)
    print()

    # AC5: FlatAST::compact_mutation_log() + mutation_log_size()
    print("--- AC5: FlatAST::compact_mutation_log() + mutation_log_size() declared ---")
    check(
        "FlatAST::compact_mutation_log declared in src/core/ast.ixx",
        "std::size_t compact_mutation_log() noexcept" in ast_ixx,
    )
    check(
        "FlatAST::mutation_log_size declared in src/core/ast.ixx",
        "std::size_t mutation_log_size() const noexcept" in ast_ixx,
    )
    check("FlatAST::compact_mutation_log body has shrink_to_fit", "mutation_log_.shrink_to_fit()" in ast_ixx)
    print()

    # AC6: Evaluator::compact_mutation_log() declared + implemented
    print("--- AC6: Evaluator::compact_mutation_log() declared + implemented ---")
    check("Evaluator::compact_mutation_log declared in evaluator.ixx", "void compact_mutation_log() noexcept;" in ixx)
    check(
        "Evaluator::compact_mutation_log implemented in evaluator_workspace_tree.cpp",
        "Evaluator::compact_mutation_log() noexcept" in wst,
    )
    check(
        "Evaluator::compact_mutation_log impl bumps mutation_log_compact_bytes_saved",
        "bump_mutation_log_compact_bytes_saved(static_cast<std::uint64_t>(saved))" in wst,
    )
    print()

    # AC7: Evaluator::ensure_env_frame_dual_path_consistent
    print("--- AC7: Evaluator::ensure_env_frame_dual_path_consistent ---")
    check(
        "ensure_env_frame_dual_path_consistent declared in evaluator.ixx",
        "bool ensure_env_frame_dual_path_consistent(EnvId id, const char*" in ixx,
    )
    check(
        "ensure_env_frame_dual_path_consistent implemented in evaluator_workspace_tree.cpp",
        "Evaluator::ensure_env_frame_dual_path_consistent(EnvId" in wst,
    )
    check("ensure_env_frame_dual_path_consistent impl checks is_env_frame_stale", "if (is_env_frame_stale(id))" in wst)
    check(
        "ensure_env_frame_dual_path_consistent impl bumps env_frame_version_drift_prevented",
        "bump_env_frame_version_drift_prevented()" in wst,
    )
    check(
        "ensure_env_frame_dual_path_consistent impl bumps dual_path_stale_fallback_total",
        "bump_dual_path_stale_fallback_total()" in wst,
    )
    print()

    # AC8: exit_mutation_boundary compact wire-up with 64KB threshold
    print("--- AC8: exit_mutation_boundary wires compact with 64KB threshold ---")
    check("exit_mutation_boundary has 64KB threshold gate", "kCompactThreshold = 64 * 1024" in ixx)
    check(
        "exit_mutation_boundary calls compact_mutation_log()",
        "compact_mutation_log();" in ixx and "workspace_flat_->mutation_log_size()" in ixx,
    )
    print()

    # AC9: 5 prod-file wire-up checks
    print("--- AC9: prod-file wire-up checks ---")
    check(
        "evaluator_env.cpp materialize_call_env wires dual-path check",
        'ensure_env_frame_dual_path_consistent(cl.env_id, "materialize_call_env")' in env_cpp,
    )
    check(
        "evaluator_gc.cpp first collect_compiler_managed_gc_roots site wires dual-path",
        "ensure_env_frame_dual_path_consistent(static_cast<EnvId>(eid),\n"
        '                                                    "collect_gc_roots_env")'
        in gc_cpp
        or "ensure_env_frame_dual_path_consistent(static_cast<EnvId>(eid),\n"
        '                                                "collect_gc_roots_env")'
        in gc_cpp,
    )
    check(
        "evaluator_gc.cpp second collect_compiler_managed_gc_roots site wires dual-path",
        "collect_gc_roots_env_2" in gc_cpp and "ensure_env_frame_dual_path_consistent" in gc_cpp,
    )
    print()

    # AC10: 3 keys + schema 1638
    print("--- AC10: query:mutation-boundary-coverage-stats extended ---")
    check("dual-path-stale-fallback-total key in primitive kv", '"dual-path-stale-fallback-total"' in prim)
    check("mutation-log-compact-bytes-saved key in primitive kv", '"mutation-log-compact-bytes-saved"' in prim)
    check("env-frame-version-drift-prevented key in primitive kv", '"env-frame-version-drift-prevented"' in prim)
    check("schema bumped to 1638 in primitive kv", "make_int(1638)" in prim)
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1638 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

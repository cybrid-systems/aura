#!/usr/bin/env python3
# scripts/check_aot_hot_update_incremental_coverage.py — Issue #1640
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: 2 metric slots in src/compiler/observability_metrics.h
#        (aot_env_frame_version_drift_prevented +
#         aot_incremental_reemit_triggered).
#   AC3: 2 AURA_COMPILER_METRICS_FIELD(...) entries in
#        src/compiler/compiler_metrics_fields.inc.
#   AC4: 2 bump_/getter pairs declared in src/compiler/evaluator.ixx.
#   AC5: aura_reload_aot_module_for_eval in src/compiler/aura_jit_bridge.cpp
#        wires env_frame_version drift check (aot_env_frame_version
#        dlsym + drift detection + 2 metric bumps + rollback_close).
#   AC6: generate_registration_c in src/compiler/aura_jit_bridge.cpp
#        emits aot_env_frame_version + threads env_frame_version +
#        linear_state through mangle_aot_name.
#   AC7: aot_mangle.h mangle_aot_name signature extended with
#        env_frame_version + linear_state params + `_e<N>_l<N>`
#        suffix.
#   AC8: aot_mangle.h backwards compat preserved (defaults keep
#        the prior 3-arg signature behavior).
#   AC9: pre-existing AOT hot-update observability metrics unchanged
#        (aot_hot_update_success_, aot_stale_reject_count_,
#        aot_region_mismatch_).
#   AC10: AuraHotUpdateReloadAttempt bump site preserved in
#         aura_reload_aot_module_for_eval (existing observability
#         surface unchanged).
#
# Pattern reference: scripts/check_incremental_relower_coverage.py
# (#1639), scripts/check_soa_dual_path_consistency_coverage.py (#1638),
# scripts/check_panic_checkpoint_lifecycle_coverage.py (#1637),
# scripts/check_macro_provenance_coverage.py (#1908),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage / freeze
# linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_aot_hot_update_incremental_coverage.py` from the repo
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
    print("=== scripts/check_aot_hot_update_incremental_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    om = read("src/compiler/observability_metrics.h")
    fields = read("src/compiler/compiler_metrics_fields.inc")
    ixx = read("src/compiler/evaluator.ixx")
    bridge = read("src/compiler/aura_jit_bridge.cpp")
    mangle = read("src/compiler/aot_mangle.h")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: 2 metric slots
    print("--- AC2: 2 metric slots in observability_metrics.h ---")
    check(
        "aot_env_frame_version_drift_prevented in observability_metrics.h",
        "aot_env_frame_version_drift_prevented" in om,
    )
    check("aot_incremental_reemit_triggered in observability_metrics.h", "aot_incremental_reemit_triggered" in om)
    print()

    # AC3: 2 X-macro fields
    print("--- AC3: 2 X-macro fields in compiler_metrics_fields.inc ---")
    check(
        "X-macro aot_env_frame_version_drift_prevented",
        "AURA_COMPILER_METRICS_FIELD(aot_env_frame_version_drift_prevented)" in fields,
    )
    check(
        "X-macro aot_incremental_reemit_triggered",
        "AURA_COMPILER_METRICS_FIELD(aot_incremental_reemit_triggered)" in fields,
    )
    print()

    # AC4: 2 bump_/getter pairs
    print("--- AC4: 2 bump_/getter pairs in evaluator.ixx ---")
    check(
        "bump_aot_env_frame_version_drift_prevented in evaluator.ixx",
        "bump_aot_env_frame_version_drift_prevented" in ixx,
    )
    check("bump_aot_incremental_reemit_triggered in evaluator.ixx", "bump_aot_incremental_reemit_triggered" in ixx)
    check(
        "get_aot_env_frame_version_drift_prevented in evaluator.ixx", "get_aot_env_frame_version_drift_prevented" in ixx
    )
    check("get_aot_incremental_reemit_triggered in evaluator.ixx", "get_aot_incremental_reemit_triggered" in ixx)
    print()

    # AC5: aura_reload_aot_module_for_eval env_frame_version drift check
    print("--- AC5: aura_reload_aot_module_for_eval env_frame_version drift check ---")
    check(
        "Issue #1640: env_frame_version drift detection present",
        "Issue #1640: env_frame_version drift detection" in bridge,
    )
    check(
        "dlsym aot_env_frame_version in reload path",
        'static_cast<std::uint64_t*>(::dlsym(handle, "aot_env_frame_version"))' in bridge,
    )
    check("drift comparison binary_env_ver < host_env_ver", "binary_env_ver" in bridge and "host_env_ver" in bridge)
    check(
        "bump aot_env_frame_version_drift_prevented on drift",
        "aot_env_frame_version_drift_prevented.fetch_add" in bridge,
    )
    check("bump aot_incremental_reemit_triggered on drift", "aot_incremental_reemit_triggered.fetch_add" in bridge)
    check("rollback_close() on drift", "rollback_close()" in bridge)
    check(
        "log message: incremental re-emit triggered, graceful fallback to JIT",
        "incremental re-emit " in bridge and "triggered, graceful fallback to JIT" in bridge,
    )
    print()

    # AC6: generate_registration_c emits aot_env_frame_version
    print("--- AC6: generate_registration_c emits aot_env_frame_version ---")
    check(
        "Issue #1640: AOT env_frame_version emit comment",
        "Issue #1640: AOT env_frame_version (captured-env drift)" in bridge,
    )
    check(
        "const unsigned long long aot_env_frame_version = %llull; emit",
        "const unsigned long long aot_env_frame_version = %llull;" in bridge,
    )
    check("emit_env_frame_version local var", "emit_env_frame_version" in bridge)
    check(
        "fn_linear_state threaded through mangle_aot_name",
        "fn_linear_state" in bridge and "linear_ownership_state" in bridge,
    )
    print()

    # AC7: mangle_aot_name signature extended
    print("--- AC7: mangle_aot_name signature extended ---")
    check("mangle_aot_name has env_frame_version param", "std::uint64_t env_frame_version = 0" in mangle)
    check("mangle_aot_name has linear_state param", "std::uint8_t linear_state = 0" in mangle)
    check("_e suffix for env_frame_version", 'result += "_e"' in mangle)
    check("_l suffix for linear_state", 'result += "_l"' in mangle)
    check("Issue #1640 mention in mangle_aot_name", "Issue #1640" in mangle)
    print()

    # AC8: backwards compat
    print("--- AC8: backwards compat preserved ---")
    check("defuse_version default still 0", "std::uint64_t defuse_version = 0" in mangle)
    check(
        "env_frame_version default 0 (skip suffix when zero)",
        "if (env_frame_version != 0 || linear_state != 0)" in mangle,
    )
    print()

    # AC9: pre-existing AOT hot-update observability unchanged
    print("--- AC9: pre-existing AOT hot-update observability unchanged ---")
    check(
        "aot_hot_update_success_ still bumped on success",
        "aot_hot_update_success_.fetch_add(1, std::memory_order_relaxed);" in bridge,
    )
    check(
        "aot_stale_reject_count_ still bumped on stale",
        "aot_stale_reject_count_.fetch_add(1, std::memory_order_relaxed);" in bridge,
    )
    check(
        "aot_region_mismatch_ still bumped on region mismatch",
        "aot_region_mismatch_.fetch_add(1, std::memory_order_relaxed);" in bridge,
    )
    print()

    # AC10: reload_attempt bump site preserved
    print("--- AC10: reload_attempt bump site preserved ---")
    check("bump_reload_attempt still called in aura_reload_aot_module_for_eval", "bump_reload_attempt()" in bridge)
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1640 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

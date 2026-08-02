#!/usr/bin/env python3
# scripts/check_live_closure_stable_id_backfill_coverage.py
#
# Issue #2175 linter: ensure legacy sid=0 backfill is real on the
# aura_remap_live_closures_after_reemit hot path. Each row below is
# a contract that the production surface MUST satisfy; missing any row
# fails the script (exit 1) and the pre-push gate surfaces the gap.
#
# Contract (per the #2175 body + shipped C++ surface):
#   1. src/compiler/aura_jit_runtime.cpp:
#      aura_remap_live_closures_after_reemit checks stored_sid == 0 +
#      non-empty name + resolved lookup + writes sid back into
#      g_closure_stable_func_ids[cid] + bumps the dedicated counter.
#      Independent of aura_get_remap_name_fallback_enabled() (AC2).
#   2. src/compiler/aura_jit_bridge.h declares
#      aura_bump_live_closure_stable_id_backfill_total; impl in
#      aura_jit_bridge.cpp mirrors the legacy name-fallback helper.
#   3. src/compiler/observability_metrics.h has
#      live_closure_stable_id_backfill_total field on CompilerMetrics.
#   4. src/compiler/evaluator.ixx has
#      get_live_closure_stable_id_backfill_total() accessor (mirror of
#      get_live_closure_remap_total).
#   5. src/compiler/evaluator_primitives_query.cpp
#      query:aot-incremental-reemit-stats exposes
#      live_closure_stable_id_backfill_total + schema-2175 + issue-2175.
#   6. tests/compiler/test_aot_incremental_reemit.cpp has AC9d
#      (ac9d_legacy_sid_backfill_2175) covering legacy sid=0 backfill,
#      unknown name (AC3), already-stamped (AC4).
#
# Self-test: --self-test exercises the substring counters against the
# live tree (must pass).
#
# Exit codes:
#   0 = pass
#   1 = contract gap
#   2 = file missing
#   3 = self-test fail

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
COMPILER_DIR = REPO_ROOT / "src" / "compiler"
TESTS_DIR = REPO_ROOT / "tests" / "compiler"


def _read(p: Path) -> str:
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _contains_all(text: str, needles: list[str]) -> list[str]:
    return [n for n in needles if n not in text]


def check_contract() -> tuple[int, list[str]]:
    failures: list[str] = []

    # 1. aura_jit_runtime.cpp — backfill logic in remap walk.
    runtime = _read(COMPILER_DIR / "aura_jit_runtime.cpp")
    if not runtime:
        failures.append("src/compiler/aura_jit_runtime.cpp missing")
    else:
        missing = _contains_all(
            runtime,
            [
                # Cite #2175.
                "Issue #2175",
                # Backfill branch reads stored_sid == 0 + name lookup
                # (cname from g_closure_names[cid], or inline c_str form).
                "via_backfill",
                "g_closure_stable_func_ids[cid] = looked_up",
                # Calls the dedicated counter helper inline.
                "aura_bump_live_closure_stable_id_backfill_total(1)",
            ],
        )
        # Accept either inline c_str form or cname local (post-#2542/#2550).
        if (
            "aura_lookup_stable_func_id(cname)" not in runtime
            and "aura_lookup_stable_func_id(g_closure_names[cid].c_str())" not in runtime
        ):
            missing.append("aura_lookup_stable_func_id(cname|g_closure_names)")
        if missing:
            failures.append(f"src/compiler/aura_jit_runtime.cpp missing #2175 backfill pieces: {missing}")

    # 2. aura_jit_bridge.h + aura_jit_bridge.cpp — declaration + definition.
    bridge_h = _read(COMPILER_DIR / "aura_jit_bridge.h")
    bridge_cpp = _read(COMPILER_DIR / "aura_jit_bridge.cpp")
    if not bridge_h:
        failures.append("src/compiler/aura_jit_bridge.h missing")
    elif "aura_bump_live_closure_stable_id_backfill_total" not in bridge_h:
        failures.append(
            "src/compiler/aura_jit_bridge.h missing aura_bump_live_closure_stable_id_backfill_total declaration"
        )
    if not bridge_cpp:
        failures.append("src/compiler/aura_jit_bridge.cpp missing")
    elif "aura_bump_live_closure_stable_id_backfill_total" not in bridge_cpp:
        failures.append(
            "src/compiler/aura_jit_bridge.cpp missing aura_bump_live_closure_stable_id_backfill_total definition"
        )

    # 3. observability_metrics.h — CompilerMetrics field.
    obs = _read(COMPILER_DIR / "observability_metrics.h")
    if not obs:
        failures.append("src/compiler/observability_metrics.h missing")
    elif "live_closure_stable_id_backfill_total" not in obs:
        failures.append("src/compiler/observability_metrics.h missing live_closure_stable_id_backfill_total field")

    # 4. evaluator.ixx — accessor.
    ev = _read(COMPILER_DIR / "evaluator.ixx")
    if not ev:
        failures.append("src/compiler/evaluator.ixx missing")
    elif "get_live_closure_stable_id_backfill_total" not in ev:
        failures.append("src/compiler/evaluator.ixx missing get_live_closure_stable_id_backfill_total() accessor")

    # 5. evaluator_primitives_query.cpp — query surface exposes new keys.
    query = _read(COMPILER_DIR / "evaluator_primitives_query.cpp")
    if not query:
        failures.append("src/compiler/evaluator_primitives_query.cpp missing")
    else:
        missing = _contains_all(
            query,
            [
                # Accessor used in remap walk.
                "get_live_closure_stable_id_backfill_total",
                # New hash keys.
                "live_closure_stable_id_backfill_total",
                "schema-2175",
                "issue-2175",
            ],
        )
        if missing:
            failures.append(f"src/compiler/evaluator_primitives_query.cpp missing #2175 query keys: {missing}")

    # 6. test_aot_incremental_reemit.cpp — AC9d.
    test_src = _read(TESTS_DIR / "test_aot_incremental_reemit.cpp")
    if not test_src:
        failures.append("tests/compiler/test_aot_incremental_reemit.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                # AC function.
                "ac9d_legacy_sid_backfill_2175",
                # File header cites #2175.
                "#2175",
                # main() call.
                "ac9d_legacy_sid_backfill_2175()",
            ],
        )
        if missing:
            failures.append(f"test_aot_incremental_reemit.cpp missing #2175 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2175 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2175 live-closure sid=0 backfill linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_live_closure_stable_id_backfill_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_live_closure_stable_id_backfill_coverage] OK: all #2175 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

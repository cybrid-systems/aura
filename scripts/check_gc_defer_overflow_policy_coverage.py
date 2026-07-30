#!/usr/bin/env python3
# scripts/check_gc_defer_overflow_policy_coverage.py
#
# Issue #2173 linter: ensure configurable kMaxArmedEvaluators + overflow
# policy (ProcessWide | HardFail | Expand) is real on the production
# surface. Each row below is a contract that the production surface
# MUST satisfy; missing any row fails the script (exit 1) and the
# pre-push gate surfaces the gap.
#
# Contract (per the #2173 body + the shipped C++ surface):
#   1. src/core/gc_hooks.h defines GcDeferOverflowPolicy enum
#      (ProcessWide | HardFail | Expand) + gc_defer_max_armed() +
#      gc_defer_overflow_policy() + AURA_GC_DEFER_MAX_ARMED env var
#      + AURA_GC_DEFER_OVERFLOW_POLICY env var.
#   2. src/core/gc_hooks.h defines g_gc_defer_arm_rejected_overflow_total
#      (file-level atomic) + try_arm_gc_defer_pending_panic_for +
#      set_gc_defer_max_armed_for_test + set_gc_defer_overflow_policy_for_test
#      + reset variants.
#   3. src/core/gc_hooks.h arm_gc_defer_pending_panic_for overflow path
#      dispatches on the active policy: ProcessWide bumps
#      g_gc_defer_table_overflow_total + process depth; HardFail bumps
#      g_gc_defer_arm_rejected_overflow_total only (no process depth
#      bump); Expand falls back to ProcessWide.
#   4. src/compiler/evaluator_primitives_obs_jit.cpp
#      query:gc-defer-reason-stats surface exposes max-armed-effective,
#      overflow-policy, arm-rejected-overflow-total, table-overflow-total,
#      schema-2173, issue-2173 (no schema break — additive keys only).
#   5. tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp has
#      AC_O1-AC_O4 covering ProcessWide overflow + HardFail reject +
#      steal clear under overflow + capacity clamp.
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
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORE_DIR = REPO_ROOT / "src" / "core"
COMPILER_DIR = REPO_ROOT / "src" / "compiler"
TESTS_DIR = REPO_ROOT / "tests" / "serve"
SECURITY_DEFAULTS = COMPILER_DIR / "security_defaults.hh"  # Issue #2338


def _read(p: Path) -> str:
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _contains_all(text: str, needles: list[str]) -> list[str]:
    return [n for n in needles if n not in text]


def check_contract() -> tuple[int, list[str]]:
    failures: list[str] = []

    # 1. gc_hooks.h — OverflowPolicy enum + getters + env vars.
    gc = _read(CORE_DIR / "gc_hooks.h")
    if not gc:
        failures.append("src/core/gc_hooks.h missing")
    else:
        missing = _contains_all(
            gc,
            [
                # Cites #2173.
                "Issue #2173",
                # Enum.
                "enum class GcDeferOverflowPolicy",
                # Getters.
                "gc_defer_max_armed",
                "gc_defer_overflow_policy",
                # Env vars.
                "AURA_GC_DEFER_MAX_ARMED",
                "AURA_GC_DEFER_OVERFLOW_POLICY",
                # Default values referenced.
                "ProcessWide",
                "HardFail",
                "Expand",
            ],
        )
        if missing:
            failures.append(f"src/core/gc_hooks.h missing #2173 contract pieces: {missing}")
        # Method signature check for gc_defer_max_armed.
        if not re.search(
            r"\[\[nodiscard\]\]\s+(?:inline\s+)?std::size_t\s+gc_defer_max_armed\s*\(\s*\)\s*noexcept",
            gc,
        ):
            failures.append("src/core/gc_hooks.h missing [[nodiscard]] std::size_t gc_defer_max_armed() noexcept")
        # Method signature check for gc_defer_overflow_policy.
        if not re.search(
            r"\[\[nodiscard\]\]\s+(?:inline\s+)?GcDeferOverflowPolicy\s+gc_defer_overflow_policy\s*\(\s*\)\s*noexcept",
            gc,
        ):
            failures.append("src/core/gc_hooks.h missing gc_defer_overflow_policy() noexcept method signature")

    # 2. gc_hooks.h — counter + try_arm + setters.
    if gc:
        missing = _contains_all(
            gc,
            [
                # Counter.
                "g_gc_defer_arm_rejected_overflow_total",
                # try_arm.
                "try_arm_gc_defer_pending_panic_for",
                # Setters.
                "set_gc_defer_max_armed_for_test",
                "set_gc_defer_overflow_policy_for_test",
                "reset_gc_defer_max_armed_for_test",
                "reset_gc_defer_overflow_policy_for_test",
            ],
        )
        if missing:
            failures.append(f"src/core/gc_hooks.h missing #2173 setters/try_arm: {missing}")
        # try_arm signature check.
        if not re.search(
            r"\[\[nodiscard\]\]\s+(?:inline\s+)?bool\s+try_arm_gc_defer_pending_panic_for\s*\(\s*void\*\s*evaluator_id\s*\)\s*noexcept",
            gc,
        ):
            failures.append("src/core/gc_hooks.h missing try_arm_gc_defer_pending_panic_for(bool) signature")

    # 3. gc_hooks.h — arm overflow path dispatches on policy.
    if gc:
        # Find the overflow path inside arm_gc_defer_pending_panic_for.
        # Look for the dispatch on GcDeferOverflowPolicy inside arm_gc_defer_pending_panic_for.
        # The simplest check: the overflow policy branch must reference HardFail + table_overflow_total + arm_rejected_overflow_total.
        if "HardFail" not in gc:
            failures.append("src/core/gc_hooks.h missing HardFail policy branch in arm overflow path")
        # Check that arm_gc_defer_pending_panic_for overflow path bumps
        # arm_rejected_overflow_total for HardFail (not just table_overflow_total).
        # We look for the pattern: policy check + arm_rejected_overflow_total bump.
        arm_fn_match = re.search(
            r"inline\s+void\s+arm_gc_defer_pending_panic_for\s*\([^)]*\)\s*noexcept\s*\{(.*?)^}",
            gc,
            re.MULTILINE | re.DOTALL,
        )
        if arm_fn_match:
            arm_body = arm_fn_match.group(1)
            if "gc_defer_overflow_policy" not in arm_body:
                failures.append(
                    "src/core/gc_hooks.h arm_gc_defer_pending_panic_for overflow path does not call gc_defer_overflow_policy()"
                )
            if "g_gc_defer_arm_rejected_overflow_total" not in arm_body:
                failures.append(
                    "src/core/gc_hooks.h arm_gc_defer_pending_panic_for overflow path does not bump g_gc_defer_arm_rejected_overflow_total on HardFail"
                )
        else:
            failures.append("src/core/gc_hooks.h could not find arm_gc_defer_pending_panic_for function body")

    # 4. evaluator_primitives_obs_jit.cpp — query prim exposes new keys.
    prim = _read(COMPILER_DIR / "evaluator_primitives_obs_jit.cpp")
    if not prim:
        failures.append("src/compiler/evaluator_primitives_obs_jit.cpp missing")
    else:
        missing = _contains_all(
            prim,
            [
                # Existing primitive (sanity).
                "query:gc-defer-reason-stats",
                # New keys (additive).
                "schema-2173",
                "issue-2173",
                "max-armed-effective",
                "overflow-policy",
                "arm-rejected-overflow-total",
                "table-overflow-total",
            ],
        )
        if missing:
            failures.append(f"src/compiler/evaluator_primitives_obs_jit.cpp missing #2173 query keys: {missing}")

    # 5. tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp has AC_O1-AC_O4.
    test_src = _read(TESTS_DIR / "test_scheduler_gc_defer_pending_panic_steal.cpp")
    if not test_src:
        failures.append("tests/serve/test_scheduler_gc_defer_pending_panic_steal.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                "ac_o1_overflow_process_wide_2173",
                "ac_o2_hardfail_arm_rejected_2173",
                "ac_o3_steal_clear_under_overflow_2173",
                "ac_o4_capacity_clamp_and_env_2173",
                # Cites #2173 in test header.
                "#2173",
            ],
        )
        if missing:
            failures.append(f"test_scheduler_gc_defer_pending_panic_steal.cpp missing #2173 AC entries: {missing}")

    # 6. Issue #2338: production default GcDefer overflow policy = HardFail.
    #    Wire-up across gc_hooks.h + security_defaults.hh +
    #    evaluator_primitives_obs_jit.cpp + test extension.
    if gc:
        missing = _contains_all(
            gc,
            [
                # Production lock atomic + setter/getter.
                "g_production_locked{0}",
                "set_gc_defer_production_locked",
                "gc_defer_production_locked",
                # Production lock wire-up in env-empty branch.
                "detail::g_production_locked.load",
                # Cite.
                "Issue #2338",
            ],
        )
        if missing:
            failures.append(f"src/core/gc_hooks.h missing #2338 production lock pieces: {missing}")
    security = _read(SECURITY_DEFAULTS)
    if not security:
        failures.append("src/compiler/security_defaults.hh missing")
    else:
        missing = _contains_all(
            security,
            [
                # Include gc_hooks.h.
                '#include "core/gc_hooks.h"',
                # Wire-up call.
                "set_gc_defer_production_locked(!dev_off)",
                # Cite.
                "Issue #2338: production lock for gc_defer_overflow_policy",
            ],
        )
        if missing:
            failures.append(f"src/compiler/security_defaults.hh missing #2338 wire-up: {missing}")
    if prim:
        missing = _contains_all(
            prim,
            [
                # New query keys.
                "gc-defer-overflow-production-locked",
                "schema-2338",
                "issue-2338",
            ],
        )
        if missing:
            failures.append(f"src/compiler/evaluator_primitives_obs_jit.cpp missing #2338 query keys: {missing}")
    if test_src:
        missing = _contains_all(
            test_src,
            [
                "ac2338_1_production_lock_roundtrip",
                "ac2338_2_query_schema",
                "ac2338_3_source_cite",
                # Cite.
                "#2338",
            ],
        )
        if missing:
            failures.append(f"test_scheduler_gc_defer_pending_panic_steal.cpp missing #2338 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2173 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2173 GC defer overflow policy linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_gc_defer_overflow_policy_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_gc_defer_overflow_policy_coverage] OK: all #2173 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

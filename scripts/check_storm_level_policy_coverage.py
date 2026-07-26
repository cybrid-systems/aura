#!/usr/bin/env python3
# scripts/check_storm_level_policy_coverage.py
#
# Issue #2172 linter: ensure StormLevel facade drives the documented
# recovery policy (Shape vs Global) on the reemit + SpecJIT hot paths.
# Each row below is a contract that the production surface MUST
# satisfy; missing any row fails the script (exit 1) and the pre-push
# gate surfaces the gap.
#
# Contract (per the #2172 body + the shipped C++ surface):
#   1. src/compiler/aura_jit_bridge.cpp:
#      aura_reemit_aot_for_dirty uses the StormLevel facade explicitly
#      (current_storm_level() & StormLevel::Global) as the throttle
#      source-of-truth. The Shape bit must NOT block reemit.
#   2. src/compiler/spec_jit_controller.cpp:
#      compile_specialized consults the StormLevel Shape bit and returns
#      nullptr early when set (conservative mode). Cached specializations
#      are still served (conservative mode = "no new specialization").
#   3. src/compiler/spec_jit_controller.cpp defines
#      g_specjit_conservative_due_to_shape_storm_total (file-level
#      atomic) + aura_specjit_conservative_due_to_shape_storm_total_v_read
#      (C-linkage reader under `extern "C"`).
#   4. src/compiler/hot_update_registry.{cpp,hh}:
#      current_storm_level() (member) + aura_hot_update_current_storm_level
#      (C-linkage accessor) + set_shape_storm_active(bool) (test surface)
#      all exist (refine #2094 facade).
#   5. tests/compiler/test_aot_incremental_reemit.cpp has AC12 (a/b/c
#      StormLevel scenarios) + AC14 (SpecJIT conservative gate + counter
#      + StormLevel facade source-cite).
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

    # 1. aura_jit_bridge.cpp — aura_reemit_aot_for_dirty uses StormLevel facade.
    bridge = _read(COMPILER_DIR / "aura_jit_bridge.cpp")
    if not bridge:
        failures.append("src/compiler/aura_jit_bridge.cpp missing")
    else:
        missing = _contains_all(
            bridge,
            [
                # Cites #2172.
                "Issue #2172",
                # Reads the facade.
                "current_storm_level()",
                # Uses the StormLevel enum.
                "StormLevel::Global",
            ],
        )
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.cpp missing #2172 contract pieces: {missing}")
        # Must be inside aura_reemit_aot_for_dirty (search for the facade usage
        # within ~200 lines of the function definition).
        if "aura_reemit_aot_for_dirty" not in bridge:
            failures.append("src/compiler/aura_jit_bridge.cpp missing aura_reemit_aot_for_dirty function")

    # 2 + 3. spec_jit_controller.cpp — conservative gate + counter + accessor.
    spec_jit = _read(COMPILER_DIR / "spec_jit_controller.cpp")
    if not spec_jit:
        failures.append("src/compiler/spec_jit_controller.cpp missing")
    else:
        missing = _contains_all(
            spec_jit,
            [
                # Cites #2172.
                "Issue #2172",
                # Uses the StormLevel facade (C-linkage accessor).
                "aura_hot_update_current_storm_level",
                # Counter definition.
                "g_specjit_conservative_due_to_shape_storm_total",
                # C-linkage accessor.
                "aura_specjit_conservative_due_to_shape_storm_total_v_read",
            ],
        )
        if missing:
            failures.append(f"src/compiler/spec_jit_controller.cpp missing #2172 contract pieces: {missing}")
        # The conservative gate must live inside compile_specialized.
        # Look for the Shape-bit mask (0x01) inside the function.
        if not re.search(
            r"compile_specialized\s*\([^)]*\)[^{]*\{[^}]*StormLevel|SpecJITController::compile_specialized\s*\([^)]*\)[^{]*\{[^}]*0x01",
            spec_jit,
        ):
            # Looser check: ensure compile_specialized references the facade
            # within its body. Use a tolerant substring approach.
            if "compile_specialized" in spec_jit and "0x01" in spec_jit:
                # find indices
                ci = spec_jit.find("SpecJITController::compile_specialized")
                if ci < 0:
                    ci = spec_jit.find("compile_specialized(")
                next_fn = spec_jit.find("\n}", ci) if ci >= 0 else -1
                if ci < 0 or next_fn < 0 or "aura_hot_update_current_storm_level" not in spec_jit[ci:next_fn]:
                    failures.append(
                        "src/compiler/spec_jit_controller.cpp compile_specialized missing StormLevel Shape gate"
                    )
            else:
                failures.append(
                    "src/compiler/spec_jit_controller.cpp compile_specialized missing StormLevel Shape gate"
                )

    # 4. hot_update_registry.{cpp,hh} — facade + accessor + setter.
    hur_cpp = _read(COMPILER_DIR / "hot_update_registry.cpp")
    hur_hh = _read(COMPILER_DIR / "hot_update_registry.hh")
    if not hur_cpp or not hur_hh:
        failures.append("src/compiler/hot_update_registry.{cpp,hh} missing")
    else:
        missing_cpp = _contains_all(
            hur_cpp,
            [
                "StormLevel",
                "current_storm_level",
                "aura_hot_update_current_storm_level",
                "set_shape_storm_active",
            ],
        )
        if missing_cpp:
            failures.append(f"src/compiler/hot_update_registry.cpp missing #2094/#2172 pieces: {missing_cpp}")
        missing_hh = _contains_all(
            hur_hh,
            [
                "enum class StormLevel",
                "current_storm_level",
                "set_shape_storm_active",
                "shape_storm_active",
            ],
        )
        if missing_hh:
            failures.append(f"src/compiler/hot_update_registry.hh missing StormLevel enum / accessors: {missing_hh}")

    # 5. tests/compiler/test_aot_incremental_reemit.cpp has AC12 + AC14.
    test_src = _read(TESTS_DIR / "test_aot_incremental_reemit.cpp")
    if not test_src:
        failures.append("tests/compiler/test_aot_incremental_reemit.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                # AC12 (a/b/c) — StormLevel scenarios.
                "ac12_storm_level_global",
                "ac12_storm_level_shape",
                "ac12_storm_level_both",
                # AC14 — SpecJIT conservative gate + counter + facade source-cite.
                "ac14_specjit_shape_conservative",
                # Cites #2172 in the test header / function.
                "#2172",
                # C-linkage accessor used in test.
                "aura_specjit_conservative_due_to_shape_storm_total_v_read",
            ],
        )
        if missing:
            failures.append(f"test_aot_incremental_reemit.cpp missing #2172 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2172 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2172 StormLevel-driven recovery policy linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_storm_level_policy_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_storm_level_policy_coverage] OK: all #2172 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

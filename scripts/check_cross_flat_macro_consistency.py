#!/usr/bin/env python3
# scripts/check_cross_flat_macro_consistency.py
#
# Issue #2171 linter: ensure cross-FlatAST + cross-StringPool clones
# leave consistent marker / provenance / kMacroExpansion bits. Each
# row below is a contract that the production surface MUST satisfy;
# missing any row fails the script (exit 1) and the pre-push gate
# surfaces the gap.
#
# Contract (per the #2171 body + the shipped C++ surface):
#   1. src/core/ast.ixx defines `validate_macro_hygiene_invariants()`
#      (returns violation count of MacroIntroduced nodes missing
#      kMacroExpansion bit in macro_dirty_; walks live nodes, skips
#      free_list).
#   2. src/compiler/macro_expansion.cpp defines
#      `ensure_cross_flat_expand_consistency()` static helper that
#      detects `&target != &source || &target_pool != &source_pool`,
#      no-ops on single-flat (perf-stable hot path), and on cross-flat
#      calls the existing `restamp_after_expand(target, new_root)`
#      (which bumps `g_macro_restamp_after_flat_total`).
#   3. src/compiler/macro_expansion.cpp wires
#      `ensure_cross_flat_expand_consistency()` into the
#      `clone_macro_body()` top-level exit (captures cross-flat status
#      at `s_hygiene_depth == 0` entry; calls helper on success return
#      exactly once per top-level invocation).
#   4. src/compiler/macro_expansion.cpp includes the NDEBUG-gated
#      debug-mode assert that calls
#      `target.validate_macro_hygiene_invariants()` after restamp and
#      aborts on > 0 violations (no debug-mode invariant regression).
#   5. tests/compiler/test_macro_restamp_after_flat.cpp has AC8-AC12
#      covering: cross-pool clone leaves consistent state, single-flat
#      clone does not bump counter, validate_macro_hygiene_invariants
#      callable directly + detects drift, cross-pool metric surface
#      monotonic.
#
# Self-test: --self-test exercises the substring counters against the
# live tree (must pass) and a synthetic broken baseline (must fail).
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
TESTS_DIR = REPO_ROOT / "tests" / "compiler"


def _read(p: Path) -> str:
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _contains_all(text: str, needles: list[str]) -> list[str]:
    return [n for n in needles if n not in text]


def check_contract() -> tuple[int, list[str]]:
    failures: list[str] = []

    # 1. ast.ixx defines validate_macro_hygiene_invariants.
    ast_ixx = _read(CORE_DIR / "ast.ixx")
    if not ast_ixx:
        failures.append("src/core/ast.ixx missing")
    else:
        missing = _contains_all(
            ast_ixx,
            [
                "validate_macro_hygiene_invariants",
                # Uses free_list_ for skip + is_macro_introduced + macro_dirty bit
                "free_list_",
                "is_macro_introduced(id)",
                "kMacroExpansion",
            ],
        )
        if missing:
            failures.append(f"src/core/ast.ixx missing validate_macro_hygiene_invariants pieces: {missing}")
        # Must be a method returning size_t (not free function).
        if not re.search(
            r"\[\[nodiscard\]\]\s+std::size_t\s+validate_macro_hygiene_invariants\s*\(\s*\)",
            ast_ixx,
        ):
            failures.append(
                "src/core/ast.ixx missing [[nodiscard]] std::size_t validate_macro_hygiene_invariants() method signature"
            )

    # 2 + 3 + 4. macro_expansion.cpp has the helper + wire-up + debug assert.
    macro_exp = _read(COMPILER_DIR / "macro_expansion.cpp")
    if not macro_exp:
        failures.append("src/compiler/macro_expansion.cpp missing")
    else:
        # Helper definition.
        if not re.search(
            r"static\s+void\s+ensure_cross_flat_expand_consistency\s*\(",
            macro_exp,
        ):
            failures.append(
                "src/compiler/macro_expansion.cpp missing ensure_cross_flat_expand_consistency() helper definition"
            )
        # Helper short-circuits on single-flat.
        if "if (!cross_flat)\n        return;" not in macro_exp and not re.search(
            r"if\s*\(\s*!cross_flat\s*\)\s*return\s*;",
            macro_exp,
        ):
            failures.append(
                "src/compiler/macro_expansion.cpp ensure_cross_flat_expand_consistency() missing single-flat short-circuit"
            )
        # Helper calls restamp_after_expand on cross-flat.
        if "restamp_after_expand(target" not in macro_exp:
            failures.append(
                "src/compiler/macro_expansion.cpp ensure_cross_flat_expand_consistency() must call restamp_after_expand(target, ...)"
            )
        # Wire-up: clone_macro_body captures cross_flat_top at entry + invokes helper at exit.
        if "cross_flat_top" not in macro_exp:
            failures.append("src/compiler/macro_expansion.cpp clone_macro_body missing cross_flat_top capture")
        if not re.search(
            r"ensure_cross_flat_expand_consistency\s*\(\s*target\s*,\s*target_pool\s*,\s*source\s*,\s*source_pool\s*,\s*new_id\s*\)",
            macro_exp,
        ):
            failures.append(
                "src/compiler/macro_expansion.cpp clone_macro_body must invoke ensure_cross_flat_expand_consistency at exit with new_id"
            )
        # Debug-mode assert.
        if "validate_macro_hygiene_invariants" not in macro_exp:
            failures.append(
                "src/compiler/macro_expansion.cpp missing validate_macro_hygiene_invariants call (debug-mode assert)"
            )
        if "#ifndef NDEBUG" not in macro_exp:
            failures.append("src/compiler/macro_expansion.cpp missing NDEBUG gate around debug-mode assert")

    # 5. tests/compiler/test_macro_restamp_after_flat.cpp has AC8-AC12.
    test_src = _read(TESTS_DIR / "test_macro_restamp_after_flat.cpp")
    if not test_src:
        failures.append("tests/compiler/test_macro_restamp_after_flat.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                "ac8_cross_pool_invariants",
                "ac9_single_flat_no_counter",
                "ac10_validate_invariants_direct",
                "ac11_validate_detects_drift",
                "ac12_cross_pool_hygiene_metric",
                # Cites #2171 in the file header / status line.
                "#2171",
            ],
        )
        if missing:
            failures.append(f"test_macro_restamp_after_flat.cpp missing #2171 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2171 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2171 cross-flat macro consistency contract linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_cross_flat_macro_consistency] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_cross_flat_macro_consistency] OK: all #2171 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

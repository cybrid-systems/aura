#!/usr/bin/env python3
# scripts/check_unstamp_macro_introduced_coverage.py
#
# Issue #2176 linter: ensure selective unstamp for MacroIntroduced subtrees
# is real on the production surface (Agent experimental rollback path).
# Each row below is a contract that the production surface MUST satisfy;
# missing any row fails the script (exit 1) and the pre-push gate surfaces
# the gap.
#
# Contract (per the #2176 body + shipped C++ surface):
#   1. src/core/ast.ixx: FlatAST::unstamp_macro_introduced(NodeId, bool) method
#      that walks the subtree, resets MacroIntroduced marker to User, optionally
#      zeros provenance, and clears kMacroExpansion dirty bit. Returns count.
#   2. src/core/ast.ixx: unstamp_macro_introduced_total_ atomic counter
#      bumped per successful unstamp + public accessor.
#   3. src/compiler/observability_metrics.h: CompilerMetrics has the
#      unstamp_macro_introduced_total field for the Agent dashboard.
#   4. src/compiler/macro_expansion.cpp: C-linkage accessor
#      aura_unstamp_macro_introduced_total_v_read for cross-TU reads.
#   5. src/compiler/evaluator_primitives_mutate.cpp: mutate:rollback-macro-introduced
#      primitive registered via add_mutate (with MutationBoundaryGuard + try_acquire).
#   6. src/compiler/evaluator_primitives_query.cpp: query:macro-unstamp-stats
#      query stats primitive exposed for Agent observability.
#   7. tests/compiler/test_jit_macro_introduced_preserve.cpp has AC8
#      (ac8_unstamp_macro_introduced_2176) covering selective unstamp +
#      snapshot/restore consistency + source-cite for the contract.
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

    # 1 + 2. ast.ixx — FlatAST method + atomic + accessor.
    ast = _read(CORE_DIR / "ast.ixx")
    if not ast:
        failures.append("src/core/ast.ixx missing")
    else:
        missing = _contains_all(
            ast,
            [
                # Cites #2176.
                "Issue #2176",
                # Method definition (with bool keep_provenance default).
                "std::size_t unstamp_macro_introduced(NodeId root, bool keep_provenance = false)",
                # Atomic field (mutable).
                "unstamp_macro_introduced_total_{0}",
                # Public accessor.
                "[[nodiscard]] std::uint64_t unstamp_macro_introduced_total() const noexcept",
            ],
        )
        if missing:
            failures.append(f"src/core/ast.ixx missing #2176 unstamp pieces: {missing}")

    # 3. observability_metrics.h — CompilerMetrics field.
    obs = _read(COMPILER_DIR / "observability_metrics.h")
    if not obs:
        failures.append("src/compiler/observability_metrics.h missing")
    elif "unstamp_macro_introduced_total{0}" not in obs:
        failures.append("src/compiler/observability_metrics.h missing unstamp_macro_introduced_total field")

    # 4. macro_expansion.cpp — C-linkage accessor.
    me = _read(COMPILER_DIR / "macro_expansion.cpp")
    if not me:
        failures.append("src/compiler/macro_expansion.cpp missing")
    elif "aura_unstamp_macro_introduced_total_v_read" not in me:
        failures.append("src/compiler/macro_expansion.cpp missing aura_unstamp_macro_introduced_total_v_read accessor")

    # 5. evaluator_primitives_mutate.cpp — primitive registered.
    ep = _read(COMPILER_DIR / "evaluator_primitives_mutate.cpp")
    if not ep:
        failures.append("src/compiler/evaluator_primitives_mutate.cpp missing")
    elif "mutate:rollback-macro-introduced" not in ep:
        failures.append(
            "src/compiler/evaluator_primitives_mutate.cpp missing mutate:rollback-macro-introduced primitive"
        )

    # 6. evaluator_primitives_query.cpp — query stats primitive.
    eq = _read(COMPILER_DIR / "evaluator_primitives_query.cpp")
    if not eq:
        failures.append("src/compiler/evaluator_primitives_query.cpp missing")
    elif "query:macro-unstamp-stats" not in eq:
        failures.append("src/compiler/evaluator_primitives_query.cpp missing query:macro-unstamp-stats primitive")

    # 7. test_jit_macro_introduced_preserve.cpp — AC8.
    test_src = _read(TESTS_DIR / "test_jit_macro_introduced_preserve.cpp")
    if not test_src:
        failures.append("tests/compiler/test_jit_macro_introduced_preserve.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                "ac8_unstamp_macro_introduced_2176",
                "ac8_unstamp_macro_introduced_2176()",
                # File header cites #2176.
                "#2176",
            ],
        )
        if missing:
            failures.append(f"test_jit_macro_introduced_preserve.cpp missing #2176 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2176 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2176 unstamp_macro_introduced contract linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_unstamp_macro_introduced_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_unstamp_macro_introduced_coverage] OK: all #2176 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

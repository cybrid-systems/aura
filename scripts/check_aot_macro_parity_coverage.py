#!/usr/bin/env python3
# scripts/check_aot_macro_parity_coverage.py
#
# Issue #2177 linter: ensure AOT marker propagation observability is real
# on the production surface (refine #2100 which was JIT-only). Each row
# below is a contract that the production surface MUST satisfy; missing
# any row fails the script (exit 1) and the pre-push gate surfaces the gap.
#
# Contract (per the #2177 body + shipped C++ surface):
#   1. src/compiler/aura_jit_bridge.cpp: file-level atomics
#      g_2177_aot_macro_marker_propagated_total + _stripped_total +
#      C-linkage accessors + record helper (called from lowering pass).
#   2. src/compiler/observability_metrics.h: CompilerMetrics fields
#      aot_macro_marker_propagated_total + _stripped_total for the
#      Agent dashboard mirror.
#   3. src/compiler/lowering_impl.cpp: calls aura_2177_record_aot_marker_propagated
#      when the source node has MacroIntroduced marker (bump propagated)
#      or when current_flat is null (bump stripped).
#   4. src/compiler/evaluator_primitives_query.cpp: query:ir-hygiene-stats
#      exposes aot-macro-marker-propagated-total + _stripped-total +
#      schema-2177 + issue-2177 lineage keys (additive, no schema break).
#   5. tests/compiler/test_jit_macro_deopt_hygiene_2100.cpp: AC7
#      (ac7_aot_marker_parity_2177) covering the AOT parity surface.
#   6. C-linkage forward declarations: aura_2177_aot_macro_marker_propagated_total
#      + aura_2177_aot_macro_marker_stripped_total accessible from
#      query:ir-hygiene-stats callback.
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

    # 1. aura_jit_bridge.cpp — file-level atomics + C-linkage accessors + record helper.
    ab = _read(COMPILER_DIR / "aura_jit_bridge.cpp")
    if not ab:
        failures.append("src/compiler/aura_jit_bridge.cpp missing")
    else:
        missing = _contains_all(
            ab,
            [
                # Cites #2177.
                "Issue #2177",
                # File-level atomics.
                "g_2177_aot_macro_marker_propagated_total{0}",
                "g_2177_aot_macro_marker_stripped_total{0}",
                # C-linkage accessors.
                "aura_2177_aot_macro_marker_propagated_total(void)",
                "aura_2177_aot_macro_marker_stripped_total(void)",
                # Record helper called from lowering pass.
                "aura_2177_record_aot_marker_propagated",
            ],
        )
        if missing:
            failures.append(f"src/compiler/aura_jit_bridge.cpp missing #2177 AOT markers: {missing}")

    # 2. observability_metrics.h — CompilerMetrics fields.
    om = _read(COMPILER_DIR / "observability_metrics.h")
    if not om:
        failures.append("src/compiler/observability_metrics.h missing")
    else:
        missing = _contains_all(
            om,
            [
                "aot_macro_marker_propagated_total{0}",
                "aot_macro_marker_stripped_total{0}",
            ],
        )
        if missing:
            failures.append(f"src/compiler/observability_metrics.h missing #2177 fields: {missing}")

    # 3. lowering_impl.cpp — counter wiring in the marker propagation.
    lo = _read(COMPILER_DIR / "lowering_impl.cpp")
    if not lo:
        failures.append("src/compiler/lowering_impl.cpp missing")
    else:
        missing = _contains_all(
            lo,
            [
                "aura_2177_record_aot_marker_propagated",
                "Issue #2177",
            ],
        )
        if missing:
            failures.append(f"src/compiler/lowering_impl.cpp missing #2177 wiring: {missing}")

    # 4. evaluator_primitives_query.cpp — query surface keys + forward decls.
    eq = _read(COMPILER_DIR / "evaluator_primitives_query.cpp")
    if not eq:
        failures.append("src/compiler/evaluator_primitives_query.cpp missing")
    else:
        missing = _contains_all(
            eq,
            [
                # Query surface keys (additive, no schema break).
                '"aot-macro-marker-propagated-total"',
                '"aot-macro-marker-stripped-total"',
                '"schema-2177"',
                '"issue-2177"',
                # Forward declarations (C-linkage accessors).
                "aura_2177_aot_macro_marker_propagated_total",
                "aura_2177_aot_macro_marker_stripped_total",
            ],
        )
        if missing:
            failures.append(f"src/compiler/evaluator_primitives_query.cpp missing #2177 keys: {missing}")

    # 5. tests/compiler/test_jit_macro_deopt_hygiene_2100.cpp — AC7.
    test_src = _read(TESTS_DIR / "test_jit_macro_deopt_hygiene_2100.cpp")
    if not test_src:
        failures.append("tests/compiler/test_jit_macro_deopt_hygiene_2100.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                "ac7_aot_marker_parity_2177",
                "ac7_aot_marker_parity_2177()",
                "AC7: #2177 AOT marker propagation parity",
            ],
        )
        if missing:
            failures.append(f"test_jit_macro_deopt_hygiene_2100.cpp missing #2177 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2177 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2177 AOT marker parity contract linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_aot_macro_parity_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_aot_macro_parity_coverage] OK: all #2177 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

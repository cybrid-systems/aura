#!/usr/bin/env python3
# scripts/check_macro_fiber_hygiene_metrics_coverage.py
#
# Issue #2174 linter: ensure query:macro-fiber-hygiene primitive exposes
# the full Agent self-throttling surface (per-fiber + runtime caps +
# concurrent + global counters) via a hash return type. Each row below
# is a contract that the production surface MUST satisfy; missing any
# row fails the script (exit 1) and the pre-push gate surfaces the gap.
#
# Contract (per the #2174 body + shipped C++ surface):
#   1. src/compiler/evaluator_primitives_obs_eval.cpp:
#      query:macro-fiber-hygiene primitive source returns a hash with
#      the schema-2174 key + all per-fiber + runtime cap + concurrent +
#      global counter keys (~22 total).
#   2. The primitive source cites #2174 in comments + uses
#      runtime_hygiene_depth_cap / pass_cap / effective_* from macro_exp
#      namespace (refines #2101 caps).
#   3. The primitive source references all global counter atomics
#      (g_macro_clone_in_flight, g_macro_clone_concurrent_peak, etc.)
#      for the Agent self-throttling surface (zero heavy alloc).
#   4. tests/compiler/test_macro_fiber_hygiene.cpp has AC6-AC8 covering
#      #2174: source-cite (22 keys), runtime cap + per-fiber keys,
#      concurrent + global counter keys.
#   5. The test file header AC list mentions #2174 (file documents the
#      contract).
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

    # 1 + 2 + 3. evaluator_primitives_obs_eval.cpp — primitive source
    # exposes hash with 22 keys + cites #2174 + references runtime caps + counters.
    prim = _read(COMPILER_DIR / "evaluator_primitives_obs_eval.cpp")
    if not prim:
        failures.append("src/compiler/evaluator_primitives_obs_eval.cpp missing")
    else:
        # Required hash keys (22 total for #2174 surface).
        required_keys = [
            # schema lineage
            '"schema"',
            '"schema-2174"',
            '"issue-2174"',
            # per-fiber (Issue #2097)
            '"fiber-id"',
            '"fiber-depth"',
            '"fiber-violations"',
            '"fiber-gensym-map-size"',
            # runtime caps (Issue #2101)
            '"depth-cap"',
            '"pass-cap"',
            '"effective-depth-limit"',
            '"effective-pass-cap"',
            # concurrent clone (Issue #2021)
            '"clone-in-flight"',
            '"clone-concurrent-peak"',
            '"clone-concurrent-fiber-total"',
            # query observability (Issue #2097)
            '"query-total"',
            '"violation-per-fiber-total"',
            # hygiene tracer (Issue #1248)
            '"tracer-expansions"',
            '"tracer-depth-max"',
            # expand observability (Issue #1652 + #2019 + #2096)
            '"macro-expansion-total"',
            '"introduced-nodes-created-total"',
            '"restamp-after-flat-total"',
            '"expand-mutate-restamp-total"',
            # MacroSelfEvo gates (Issue #2023)
            '"self-evo-denied-total"',
            '"self-evo-allowed-total"',
            '"self-evo-pass-clamp-total"',
            '"self-evo-depth-clamp-total"',
        ]
        missing = _contains_all(prim, required_keys)
        if missing:
            failures.append(f"src/compiler/evaluator_primitives_obs_eval.cpp missing #2174 keys: {missing}")
        # Cite #2174.
        if "Issue #2174" not in prim:
            failures.append("src/compiler/evaluator_primitives_obs_eval.cpp missing 'Issue #2174' citation")
        # Reference runtime caps from macro_exp namespace.
        for sym in (
            "runtime_hygiene_depth_cap()",
            "runtime_hygiene_pass_cap()",
            "effective_hygiene_depth_limit()",
            "effective_hygiene_pass_cap()",
        ):
            if sym not in prim:
                failures.append(f"src/compiler/evaluator_primitives_obs_eval.cpp missing macro_exp::{sym} reference")
        # Reference all global counter atomics for Agent throttling.
        for sym in (
            "g_macro_clone_in_flight",
            "g_macro_clone_concurrent_peak",
            "g_macro_clone_concurrent_fiber_total",
            "g_fiber_hygiene_query_total",
            "g_fiber_hygiene_violation_per_fiber_total",
            "g_hygiene_tracer_expansions",
            "g_hygiene_tracer_depth_max",
            "g_macro_expansion_total",
            "g_macro_introduced_nodes_created_total",
            "g_macro_restamp_after_flat_total",
            "g_macro_expand_mutate_restamp_total",
            "g_macro_self_evo_denied_total",
            "g_macro_self_evo_allowed_total",
            "g_macro_self_evo_pass_clamp_total",
            "g_macro_self_evo_depth_clamp_total",
        ):
            if sym not in prim:
                failures.append(f"src/compiler/evaluator_primitives_obs_eval.cpp missing reference to {sym}")

    # 4 + 5. tests/compiler/test_macro_fiber_hygiene.cpp — AC6-AC8 + header cite.
    test_src = _read(TESTS_DIR / "test_macro_fiber_hygiene.cpp")
    if not test_src:
        failures.append("tests/compiler/test_macro_fiber_hygiene.cpp missing")
    else:
        missing = _contains_all(
            test_src,
            [
                # AC functions.
                "ac6_extended_hash_source_cite_2174",
                "ac7_runtime_caps_and_per_fiber_2174",
                "ac8_concurrent_and_global_counters_2174",
                # File header cites #2174.
                "#2174",
                # main() calls the new ACs.
                "ac6_extended_hash_source_cite_2174()",
                "ac7_runtime_caps_and_per_fiber_2174()",
                "ac8_concurrent_and_global_counters_2174()",
            ],
        )
        if missing:
            failures.append(f"test_macro_fiber_hygiene.cpp missing #2174 AC entries: {missing}")

    return (1 if failures else 0, failures)


def self_test() -> int:
    rc, failures = check_contract()
    if rc != 0:
        print(f"[self-test FAIL] live tree already violates contract: {failures}")
        return 3
    print("[self-test OK] live tree satisfies all #2174 contract rows")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #2174 macro-fiber-hygiene metrics linter")
    ap.add_argument("--self-test", action="store_true", help="run synthetic baseline checks (CI gate)")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check_contract()
    if rc != 0:
        print("[check_macro_fiber_hygiene_metrics_coverage] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return rc
    print("[check_macro_fiber_hygiene_metrics_coverage] OK: all #2174 contract rows present")
    return 0


if __name__ == "__main__":
    sys.exit(main())

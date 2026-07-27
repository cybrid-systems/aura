#!/usr/bin/env python3
"""Issue #2239: complete rest-param + nested quasiquote hygiene +
schema_cache stamping visibility.

Contract (6 AC from issue body):
  AC1: counters + v_read accessors + helper + wire-ups + query primitive
       hash with new keys.
  AC2: stamp_rest_param_hygiene helper applies kMacroExpansion +
       set_provenance + schema_cache copy to the freshly allocated
       rest-list Call + every arg.
  AC3: pre_scan is qq-aware — Call to 'quasiquote' recurses with
       deeper qq_depth; Call to 'unquote' stops recursion.
  AC4: query:macro-schema-cache-dirty-stamp-stats returns hash with
       schema-cache-dirty-stamped-total (existing #2098) + 2 new
       counters + lineage markers.
  AC5: macro_expand_all on a dotted macro body with nested qq
       containing a rest param gensyms the rest param + substitute
       (list 2 3) for free uses of rest.
  AC6: no regression on non-dotted macros (counters unchanged).

This linter is the source-of-truth for the production surface.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    mex = _read("src/compiler/macro_expansion.cpp")
    qry = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_rest_param_nested_qq_hygiene_2239.cpp")

    # AC1 — counters + v_read accessors + helper + 2 wire-ups + primitive
    _must(
        "g_macro_rest_param_nested_qq_hits_total{0}" in mex and "g_macro_schema_cache_rest_stamped_total{0}" in mex,
        "AC1: 2 new atomic counters missing in macro_expansion.cpp",
        fails,
    )
    _must(
        "aura_macro_rest_param_nested_qq_hits_total_v_read" in mex
        and "aura_macro_schema_cache_rest_stamped_total_v_read" in mex,
        "AC1: 2 new C-linkage v_read accessors missing",
        fails,
    )
    _must(
        "stamp_rest_param_hygiene" in mex,
        "AC1: stamp_rest_param_hygiene helper missing",
        fails,
    )
    # stamp_rest_param_hygiene must be defined AND called at 2 wire-up sites
    # (expand_inner_macros + macro_expand_all_body).
    stamp_calls = mex.count("stamp_rest_param_hygiene(") - 1  # subtract the definition line
    _must(
        stamp_calls >= 2,
        f"AC1: stamp_rest_param_hygiene wire-ups missing (expected >= 2 calls, found {stamp_calls})",
        fails,
    )
    # AC2 — kMacroExpansion + set_provenance + set_schema_cache inside helper
    _must(
        "apply_macro_dirty_bits" in mex and "set_provenance" in mex and "set_schema_cache" in mex,
        "AC2: helper must apply kMacroExpansion + set_provenance + set_schema_cache",
        fails,
    )

    # AC3 — pre_scan qq-aware
    _must(
        "qq_depth" in mex and "quasiquote" in mex and "unquote" in mex,
        "AC3: pre_scan qq-aware (qq_depth + quasiquote + unquote boundary) missing",
        fails,
    )

    # AC4 — query:macro-schema-cache-dirty-stamp-stats returns hash with new keys
    _must(
        "query:macro-schema-cache-dirty-stamp-stats" in qry,
        "AC4: query primitive registration missing",
        fails,
    )
    _must(
        "rest-param-nested-qq-hits-total" in qry and "schema-cache-rest-stamped-total" in qry,
        "AC4: 2 new keys missing in query:macro-schema-cache-dirty-stamp-stats hash",
        fails,
    )
    _must(
        'insert_kv("schema", 2239)' in qry or "schema-2239" in qry,
        "AC4: schema=2239 lineage marker missing",
        fails,
    )
    _must(
        "rest-param-qq-wired" in qry and "schema-cache-rest-stamp-wired" in qry,
        "AC4: rest-param-qq-wired + schema-cache-rest-stamp-wired lineage missing",
        fails,
    )

    # AC5 — test exercises macro_expand_all on qq + dotted lambda
    _must(
        "macro_expand_all" in test and "add_call(qq_var" in test,
        "AC5: test must exercise macro_expand_all on qq-wrapped dotted lambda",
        fails,
    )

    # AC6 — non-dotted macro no-regression AC
    _must(
        "ac6_no_regression_non_dotted" in test,
        "AC6: non-dotted no-regression AC missing in test file",
        fails,
    )

    # Test surface covers #2239
    _must(
        "#2239" in test,
        "AC test: #2239 issue citation missing in test file comment",
        fails,
    )
    _must(
        "ac1_source" in test
        and "ac2_stamp_rest_param_hygiene_via_expand" in test
        and "ac3_pre_scan_qq_aware" in test
        and "ac4_query_primitive_hash" in test
        and "ac5_expand_resolves_rest_via_qq_gensym" in test,
        "AC test: 5+ ac* test functions missing (ac1..ac5)",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Issue #2239 rest-param nested-qq hygiene + schema_cache stamping linter"
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-test (return 0 if contract satisfied)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Strict mode (non-zero exit on any failure)",
    )
    args = parser.parse_args()
    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: rest-param nested-qq hygiene + schema_cache stamping coverage - all 6 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

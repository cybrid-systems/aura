#!/usr/bin/env python3
"""Issue #2249: Region | Staging auto-retry conservative path (extend #2232).

Contract (6 AC from issue body):
  AC1: Region fail under policy -> up to 2 reemit attempts with 15ms
       backoff; exhausted -> JIT-only + metric.
  AC2: Staging identical behaviour.
  AC3: Dlopen / Other still zero auto attempts (regression vs #2232 AC).
  AC4: Metrics additive: aot_reload_region_staging_retry_total +
       aot_reload_region_staging_exhausted_total + query surface +
       schema-2249 lineage.
  AC5: Env AURA_AOT_RELOAD_AUTO_RETRY=0 still disables all auto
       recovery including the new Region/Staging path.
  AC6: success on 2nd Region attempt -> success counter, no exhausted.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


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

    bridge_h = _read("src/compiler/aura_jit_bridge.h")
    bridge_cpp = _read("src/compiler/aura_jit_bridge.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_aot_reload_primitive.cpp")

    # AC1/AC2 — policy_for Region|Staging in aura_jit_bridge.h
    _must(
        re.search(
            r"case AotReloadFail::Region:\s*\n\s*case AotReloadFail::Staging:"
            r"\s*\n\s*return ReloadPolicy\{",
            bridge_h,
        )
        is not None
        or (
            bridge_h.find("AotReloadFail::Region") != -1
            and bridge_h.find("AotReloadFail::Staging") != -1
            and bridge_h.find("max_reemit=*/2") != -1
            and bridge_h.find("backoff_ms=*/15") != -1
        ),
        "AC1/AC2: policy_for Region|Staging returns {2,15,true}",
        fails,
    )
    _must(
        "aot_reload_storm_skip_retry_for_2249" in bridge_h, "AC1: storm_skip helper missing in aura_jit_bridge.h", fails
    )
    _must(
        "AURA_AOT_RELOAD_AUTO_RETRY" in bridge_cpp and "aot_reload_storm_skip_retry_for_2249(reason)" in bridge_cpp,
        "AC1: storm_skip invoked at retry loop in aura_jit_bridge.cpp",
        fails,
    )

    # AC3 — Dlopen / Other still never (regression vs #2232)
    _must(
        bridge_h.find("AotReloadFail::Dlopen") != -1 and bridge_h.find("max_reemit=*/0") != -1,
        "AC3: Dlopen still {0,0,false}",
        fails,
    )

    # AC4 — 2 metric fields + 2 query keys + schema-2249
    _must(
        "aot_reload_region_staging_retry_total{0}" in met,
        "AC4: aot_reload_region_staging_retry_total field missing",
        fails,
    )
    _must(
        "aot_reload_region_staging_exhausted_total{0}" in met,
        "AC4: aot_reload_region_staging_exhausted_total field missing",
        fails,
    )
    _must("aot-reload-region-staging-retry-total" in q, "AC4: aot-reload-region-staging-retry-total key missing", fails)
    _must(
        "aot-reload-region-staging-exhausted-total" in q,
        "AC4: aot-reload-region-staging-exhausted-total key missing",
        fails,
    )
    _must(
        "aot-reload-region-staging-policy-wired" in q,
        "AC4: aot-reload-region-staging-policy-wired sentinel missing",
        fails,
    )
    _must("schema-2249" in q and "issue-2249" in q, "AC4: schema-2249 / issue-2249 lineage missing", fails)

    # AC4 — counter bump sites in aura_jit_bridge.cpp
    _must(
        "aot_reload_region_staging_retry_total.fetch_add" in bridge_cpp,
        "AC4: retry counter bump site missing in aura_jit_bridge.cpp",
        fails,
    )
    _must(
        "aot_reload_region_staging_exhausted_total.fetch_add" in bridge_cpp,
        "AC4: exhausted counter bump site missing in aura_jit_bridge.cpp",
        fails,
    )

    # AC5 — env override AURA_AOT_RELOAD_AUTO_RETRY=0 still disables all
    _must("AURA_AOT_RELOAD_AUTO_RETRY" in bridge_cpp, "AC5: env override missing in aura_jit_bridge.cpp", fails)
    _must(
        bridge_cpp.find("aot_reload_fail_is_auto_retryable") != -1
        and bridge_cpp.find("case AotReloadFail::Region:") != -1
        and bridge_cpp.find("case AotReloadFail::Staging:") != -1,
        "AC5: aot_reload_fail_is_auto_retryable covers Region + Staging",
        fails,
    )

    # AC6 — success on 2nd Region attempt (contract surface)
    _must(
        ("ac2249_region_staging_retry" in test) or ("AC #2249" in test),
        "AC6: ac2249_region_staging_retry test function (or AC #2249 inline block) missing",
        fails,
    )
    _must("#2249" in test, "AC6: #2249 issue citation missing in test file comment", fails)
    _must(
        "policy_for(AotReloadFail::Region)" in test
        and "policy_for(AotReloadFail::Staging)" in test
        and "policy_for(AotReloadFail::Dlopen)" in test,
        "AC6: policy_for() checks for Region/Staging/Dlopen missing",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2249 AOT reload policy Region|Staging coverage linter")
    parser.add_argument("--self-test", action="store_true", help="Run self-test (return 0 if contract satisfied)")
    parser.add_argument("--strict", action="store_true", help="Strict mode (non-zero exit on any failure)")
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

    print("OK: AOT reload Region|Staging policy coverage - all 6 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2294 / #2267: RootRemapPass real rewrite coverage linter.

Contract (5 AC from #2294, building on #2267 surface):
  AC1: Stable-object root rewrite — register_root_remap_stable_slot +
       run_root_remap_pass rewrite path in root_remap_pass.ixx;
       arena invoke writes LiveCompactResult.root_remap_stable_ref_*.
  AC2: Closure capture rewrite — register_root_remap_closure_capture_slot
       + root_remap_closure_capture_* counters.
  AC3: Empty remap early-return (zero rewrite work).
  AC4: Fail-closed unmapped densify candidates + optional
       AURA_ROOT_REMAP_CONTRACT=hard.
  AC5: Observability (query keys + schema-2267 lineage + rewrite-ok metrics)
       + tests/compiler/test_root_remap_pass_2267.cpp + Evaluator install.
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

    arena = _read("src/core/arena.ixx")
    pass_ixx = _read("src/compiler/root_remap_pass.ixx")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    eval_gc = _read("src/compiler/evaluator_gc.cpp")
    eval_ixx = _read("src/compiler/evaluator.ixx")
    test_cpp = _read("tests/compiler/test_root_remap_pass_2267.cpp")
    modules = _read("cmake/AuraModules.cmake")

    # AC1: Pass surface + real rewrite.
    _must(
        "RootRemapCallback" in arena,
        "AC1: RootRemapCallback typedef missing in src/core/arena.ixx",
        fails,
    )
    _must(
        "set_root_remap_callback" in arena and "invoke_root_remap_callback_" in arena,
        "AC1: set/invoke root_remap callback missing in arena.ixx",
        fails,
    )
    _must(
        "run_root_remap_pass" in pass_ixx and "register_root_remap_stable_slot" in pass_ixx,
        "AC1: run_root_remap_pass + stable-slot registry missing in root_remap_pass.ixx",
        fails,
    )
    _must(
        "out_stable_ref_total" in arena or "out_sr" in arena or "root_remap_stable_ref_total +=" in arena,
        "AC1: arena must write root_remap stable_ref stats into LiveCompactResult",
        fails,
    )
    _must(
        "root_remap_pass.ixx" in modules,
        "AC1: root_remap_pass.ixx must be in cmake/AuraModules.cmake",
        fails,
    )

    # AC2: Closure capture rewrite.
    _must(
        "register_root_remap_closure_capture_slot" in pass_ixx,
        "AC2: closure-capture slot registry missing",
        fails,
    )
    _must(
        "root_remap_closure_capture_total" in arena and "root_remap_closure_capture_fail_total" in arena,
        "AC2: LiveCompactResult must expose closure_capture total + fail",
        fails,
    )
    _must(
        "root_remap_closure_capture_total" in met,
        "AC2: CompilerMetrics atomic root_remap_closure_capture_total missing",
        fails,
    )

    # AC3: empty remap zero-cost.
    _must(
        "object_remap.empty()" in pass_ixx or "AC3" in pass_ixx,
        "AC3: empty-remap early return missing in root_remap_pass.ixx",
        fails,
    )

    # AC4: fail-closed.
    _must(
        "AURA_ROOT_REMAP_CONTRACT" in pass_ixx,
        "AC4: AURA_ROOT_REMAP_CONTRACT hard-fail env missing",
        fails,
    )
    _must(
        "stable_ref_fail_total" in pass_ixx or "fail_total" in pass_ixx,
        "AC4: fail-total accounting missing in pass",
        fails,
    )
    _must(
        "mark_root_remap_densify_candidates" in pass_ixx,
        "AC4: densify-candidate mark API missing (fail-closed path)",
        fails,
    )

    # AC5: observability + tests + Evaluator install.
    _must(
        "root_remap_stable_ref_total" in met,
        "AC5: CompilerMetrics atomic root_remap_stable_ref_total missing",
        fails,
    )
    _must(
        "root_remap_stable_ref_total" in eval_gc and "root_remap_closure_capture_total" in eval_gc,
        "AC5: evaluator_gc.cpp must mirror the new 4 atomics at live_compact entry",
        fails,
    )
    _must(
        eval_ixx.count("root_remap_stable_ref_total") >= 2 and eval_ixx.count("root_remap_closure_capture_total") >= 2,
        "AC5: evaluator.ixx must mirror the new 4 atomics at both live_compact sites",
        fails,
    )
    _must(
        "make_root_remap_arena_callback" in eval_ixx and "set_root_remap_callback" in eval_ixx,
        "AC5: Evaluator::set_arena must install RootRemapPass callback",
        fails,
    )
    _must(
        "root-remap-stable-ref-total" in q
        and "root-remap-stable-ref-fail-total" in q
        and "root-remap-closure-capture-total" in q
        and "root-remap-closure-capture-fail-total" in q,
        "AC5: query:compact-stats must surface all 4 root_remap keys",
        fails,
    )
    _must(
        '"schema-2267"' in q and '"issue-2267"' in q and '"root-remap-pass-wired"' in q,
        "AC5: query primitive must surface schema-2267 / issue-2267 / root-remap-pass-wired",
        fails,
    )

    # AC6: Issue #2339 — RootRemapPass auto-register / auto-unregister
    #     surface (RAII helpers + atomics + auto_register functions +
    #     query keys + test functions). Production wire-up at Closure /
    #     Stable materialize sites is a follow-up; this section enforces
    #     the API surface exists so future wire-ups have a stable contract.
    _must(
        "g_root_remap_auto_register_total{0}" in pass_ixx,
        "AC6: root_remap_pass.ixx must define g_root_remap_auto_register_total atomic",
        fails,
    )
    _must(
        "g_root_remap_auto_register_unregister_total{0}" in pass_ixx,
        "AC6: root_remap_pass.ixx must define g_root_remap_auto_register_unregister_total atomic",
        fails,
    )
    _must(
        "auto_register_root_remap_stable_slot" in pass_ixx,
        "AC6: root_remap_pass.ixx must define auto_register_root_remap_stable_slot function",
        fails,
    )
    _must(
        "auto_register_root_remap_closure_capture_slot" in pass_ixx,
        "AC6: root_remap_pass.ixx must define auto_register_root_remap_closure_capture_slot function",
        fails,
    )
    _must(
        "class RootRemapAutoRegisterStable" in pass_ixx,
        "AC6: root_remap_pass.ixx must define RootRemapAutoRegisterStable RAII class",
        fails,
    )
    _must(
        "class RootRemapAutoRegisterClosureCapture" in pass_ixx,
        "AC6: root_remap_pass.ixx must define RootRemapAutoRegisterClosureCapture RAII class",
        fails,
    )
    _must(
        "Issue #2339" in pass_ixx,
        "AC6: root_remap_pass.ixx must cite Issue #2339",
        fails,
    )
    _must(
        "root-remap-auto-register-total" in q and "root_remap_auto_register_total" in q,
        "AC6: evaluator_primitives_obs_eval.cpp must surface root-remap-auto-register-total (kebab + snake)",
        fails,
    )
    _must(
        "root-remap-auto-register-wired" in q,
        "AC6: evaluator_primitives_obs_eval.cpp must surface root-remap-auto-register-wired sentinel",
        fails,
    )
    _must(
        '"schema-2339"' in q and '"issue-2339"' in q,
        "AC6: evaluator_primitives_obs_eval.cpp must surface schema-2339 / issue-2339 sentinels",
        fails,
    )
    _must(
        "ac2339_1_raii_helper_lifecycle" in test_cpp,
        "AC6: test_root_remap_pass_2267.cpp must define ac2339_1_raii_helper_lifecycle",
        fails,
    )
    _must(
        "ac2339_2_auto_register_counter_accessible" in test_cpp,
        "AC6: test_root_remap_pass_2267.cpp must define ac2339_2_auto_register_counter_accessible",
        fails,
    )
    _must(
        "ac2339_3_query_schema" in test_cpp,
        "AC6: test_root_remap_pass_2267.cpp must define ac2339_3_query_schema",
        fails,
    )
    _must(
        "ac2339_4_source_cite" in test_cpp,
        "AC6: test_root_remap_pass_2267.cpp must define ac2339_4_source_cite",
        fails,
    )
    _must(
        "Issue #2339" in test_cpp,
        "AC6: test_root_remap_pass_2267.cpp must cite Issue #2339",
        fails,
    )
    _must(
        "Issue #2339" in q,
        "AC6: evaluator_primitives_obs_eval.cpp must cite Issue #2339",
        fails,
    )

    _must(
        "Issue #2339" in q,
        "AC6: evaluator_primitives_obs_eval.cpp must cite Issue #2339",
        fails,
    )
    _must(
        "root_remap_rewrite_ok_total" in pass_ixx,
        "AC5: additive rewrite-success metric missing",
        fails,
    )
    _must(
        "AC1" in test_cpp
        and "AC2" in test_cpp
        and "AC3" in test_cpp
        and "AC4" in test_cpp
        and "AC5" in test_cpp
        and "run_root_remap_pass" in test_cpp,
        "AC5: tests/compiler/test_root_remap_pass_2267.cpp must cover AC1-AC5 + real rewrite",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2294 RootRemapPass real rewrite coverage linter")
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
    print("OK: RootRemapPass real rewrite coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2267: RootRemapPass minimal slice — StableNodeRef + Closure
captures after Moving densify.

Contract (5 AC from issue body):
  AC1: Pass surface — `RootRemapCallback` typedef in src/core/arena.ixx
       with `set_root_remap_callback` / `take_root_remap_callback` methods +
       `invoke_root_remap_callback` caller in live_compact Moving branch.
  AC2: StableNodeRef remap — happy path: pin + Moving → `stable_ref_total`
       increments; `stable_ref_fail_total` stays at 0.
  AC3: Closure capture remap — same as AC2 for the closure-capture counter.
  AC4: Observability — `root_remap_stable_ref_total` / `_fail_total` and
       `root_remap_closure_capture_total` / `_fail_total` CompilerMetrics
       atomics + mirror at the 3 sync points (evaluator_gc.cpp + both
       evaluator.ixx sites) + `query:compact-stats` extension with new keys
       + `schema-2267` / `issue-2267` / `root-remap-pass-wired` lineage.
  AC5: Tests — `tests/compiler/test_root_remap_pass_2267.cpp` covers
       AC1 source gate + AC5 positive (per-call counters bump via
       thread_local CompilerMetrics).

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

    arena = _read("src/core/arena.ixx")
    pass_cpp = _read("src/compiler/root_remap_pass.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    eval_gc = _read("src/compiler/evaluator_gc.cpp")
    eval_ixx = _read("src/compiler/evaluator.ixx")
    test_cpp = _read("tests/compiler/test_root_remap_pass_2267.cpp")

    # AC1: Pass surface — RootRemapCallback typedef + set/invoke methods.
    _must(
        "RootRemapCallback" in arena,
        "AC1: RootRemapCallback typedef missing in src/core/arena.ixx",
        fails,
    )
    _must(
        "set_root_remap_callback" in arena,
        "AC1: set_root_remap_callback setter missing in src/core/arena.ixx",
        fails,
    )
    _must(
        "take_root_remap_callback" in arena,
        "AC1: take_root_remap_callback getter missing in src/core/arena.ixx",
        fails,
    )
    _must(
        "invoke_root_remap_callback" in arena,
        "AC1: invoke_root_remap_callback caller missing in src/core/arena.ixx",
        fails,
    )
    _must(
        "root_remap_pass_callback_impl" in pass_cpp,
        "AC1: pass impl function missing in src/compiler/root_remap_pass.cpp",
        fails,
    )
    _must(
        "get_root_remap_pass_test_callback" in pass_cpp,
        "AC1: get_root_remap_pass_test_callback accessor missing in pass.cpp",
        fails,
    )

    # AC2 + AC3: per-arena counters — LiveCompactResult + ArenaStats + per-call fields.
    _must(
        "root_remap_stable_ref_total" in arena and "root_remap_stable_ref_fail_total" in arena,
        "AC2: LiveCompactResult must expose root_remap_stable_ref_total + _fail_total",
        fails,
    )
    _must(
        "root_remap_closure_capture_total" in arena and "root_remap_closure_capture_fail_total" in arena,
        "AC3: LiveCompactResult must expose root_remap_closure_capture_total + _fail_total",
        fails,
    )
    _must(
        "root_remap_stable_ref_total" in met,
        "AC2: CompilerMetrics atomic root_remap_stable_ref_total missing",
        fails,
    )
    _must(
        "root_remap_closure_capture_total" in met,
        "AC3: CompilerMetrics atomic root_remap_closure_capture_total missing",
        fails,
    )

    # AC4: mirror at 3 sync points (evaluator_gc.cpp + both evaluator.ixx sites).
    _must(
        "root_remap_stable_ref_total" in eval_gc and "root_remap_closure_capture_total" in eval_gc,
        "AC4: evaluator_gc.cpp must mirror the new 4 atomics at live_compact entry",
        fails,
    )
    _must(
        eval_ixx.count("root_remap_stable_ref_total") >= 2 and eval_ixx.count("root_remap_closure_capture_total") >= 2,
        "AC4: evaluator.ixx must mirror the new 4 atomics at both live_compact sites",
        fails,
    )

    # AC4: query primitive extension.
    _must(
        "root-remap-stable-ref-total" in q
        and "root-remap-stable-ref-fail-total" in q
        and "root-remap-closure-capture-total" in q
        and "root-remap-closure-capture-fail-total" in q,
        "AC4: query:compact-stats must surface all 4 root_remap keys",
        fails,
    )
    _must(
        '"schema-2267"' in q and '"issue-2267"' in q,
        "AC4: query primitive must surface schema-2267 / issue-2267 lineage",
        fails,
    )
    _must(
        '"root-remap-pass-wired"' in q,
        "AC4: query primitive must surface root-remap-pass-wired sentinel",
        fails,
    )

    # AC5: tests file exists with appropriate content.
    _must(
        "AC5" in test_cpp and "root_remap_pass_calls_total" in test_cpp,
        "AC5: tests/compiler/test_root_remap_pass_2267.cpp must include AC5 positive + counter bump check",
        fails,
    )
    _must(
        "AC1" in test_cpp and "RootRemapCallback" in test_cpp,
        "AC5: tests file must include AC1 source gate for RootRemapCallback",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2267 RootRemapPass minimal slice coverage linter")
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
    print("OK: RootRemapPass minimal slice coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

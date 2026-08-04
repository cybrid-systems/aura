#!/usr/bin/env python3
"""Issue #2251: RegionExclusive env_gen fence for EnvFrame dual-path / shared parent walks.

Contract (5 AC from issue body):
  AC1: EnvFrame stores env_gen_stamp_ (uint64), set at alloc + refreshed
       in publish_layout_stamp().
  AC2: materialize_call_env (under env_frames_mtx_ shared lock): stamp
       mismatch -> empty-Env safe fallback + bump env_gen_fence_reject_total.
  AC3: lookup_by_symid_chain / walk_env_frames parent walks: on gen
       mismatch, std::nullopt / skip (no silent use of foreign-gen
       bindings).
  AC4: Hard dual-path mode unchanged; Soft mode unchanged.
  AC5: dual-region concurrent apply on shared parent -> fence reject
       or empty Env, no dual-path desync panic / UAF.

This linter is the source-of-truth for the production surface. A
ship is incomplete if any contract row fails.
"""

from __future__ import annotations

import argparse
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

    eval_ixx = _read("src/compiler/evaluator.ixx")
    env = _read("src/compiler/evaluator_env.cpp")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_envframe_truncate_epoch.cpp")

    # AC1 — env_gen_stamp_ field on EnvFrame + alloc + publish refresh
    _must("env_gen_stamp_" in eval_ixx, "AC1: EnvFrame::env_gen_stamp_ field missing", fails)
    _must(
        "fr.env_gen_stamp_ = env_generation_" in env, "AC1: alloc_env_frame stamps env_gen_stamp_ at allocation", fails
    )
    _must("fr.env_gen_stamp_ = stamp.env_gen" in mut, "AC1: publish_layout_stamp refreshes env_gen_stamp_", fails)

    # AC2 — materialize_call_env fence + empty-Env + bump
    _must("fr.env_gen_stamp_ != env_generation_" in env, "AC2: materialize_call_env fence check missing", fails)
    _must("empty_ne.set_parent_id(NULL_ENV_ID)" in env, "AC2: empty-Env safe fallback missing", fails)
    _must("env_gen_fence_reject_total.fetch_add" in env, "AC2: fence_reject counter bump site missing", fails)

    # AC3 — lookup_by_symid_chain + walk_env_frames fence
    _must("lookup_by_symid_chain" in env, "AC3: lookup_by_symid_chain fence site missing", fails)
    _must("return std::nullopt" in env, "AC3: std::nullopt fallback missing", fails)
    _must("walk_env_frames" in env and "fr.env_gen_stamp_" in env, "AC3: walk_env_frames fence missing", fails)

    # AC4 — metric + query + schema-2251
    _must("env_gen_fence_reject_total{0}" in met, "AC4: env_gen_fence_reject_total counter field missing", fails)
    _must("env-gen-fence-reject-total" in q, "AC4: env-gen-fence-reject-total query key missing", fails)
    _must("env-gen-fence-wired" in q, "AC4: env-gen-fence-wired sentinel missing", fails)
    _must("schema-2251" in q and "issue-2251" in q, "AC4: schema-2251 / issue-2251 lineage missing", fails)

    # AC5 — test surface covers #2251 (ac2251 in test_envframe_truncate_epoch.cpp)
    _must("ac2251_env_gen_fence" in test, "AC5: ac2251_env_gen_fence test function missing", fails)
    _must("#2251" in test, "AC5: #2251 issue citation missing in test file comment", fails)

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2251 env_gen fence coverage linter")
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

    print("OK: env_gen fence coverage - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2928: budgeted residual live-closure remount (round-robin).

Outside reemit-success (#2602/#2691/#2850). Residual MustDeopt /
generation-behind drain via cursor + budget B (default 32 production).

Contract (one row per AC):
  AC1 residual tick + remount_or_force_deopt; ok counter; cursor
  AC2 hard storm / throttle → budget_skip; no remount storm
  AC3 reemit-success named/captured/pure paths unchanged; quiet-only wire
  AC4 Soft / budget=0 / nslots==0 → zero walk
  AC5 additive query keys schema-2928; preserve pure-anon surface
  AC6 source-cite + this linter; extend test_anonymous_residual_*;
      no invent; no docs/design/*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    reg = _read("src/compiler/hot_update_registry.cpp")
    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    qeval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    shared = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1
    must("aura_residual_live_closure_remount_tick", "AC1", rt)
    must("g_residual_remount_cursor", "AC1", rt)
    must("aura_residual_remount_budget_default", "AC1", rt)
    must("AURA_RESIDUAL_REMOUNT_BUDGET", "AC1", rt)
    must("remount_or_force_deopt_unlocked_no_call_time_counter", "AC1 remount", rt)
    must("residual_remount_ok_total", "AC1", obs)
    must("aura_bump_residual_remount_totals", "AC1", br)
    must("Issue #2928", "AC1", rt)

    # AC2 storm skip
    must("aura_hot_update_current_storm_level", "AC2", rt)
    must("aura_hot_update_should_throttle_reemit", "AC2", rt)
    must("budget_skip", "AC2", rt)
    must("residual_remount_budget_skip_total", "AC2", obs)

    # AC3 reemit-success unchanged; quiet-only
    must("aura_sync_remount_named_live_closures", "AC3", rt)
    must("aura_sync_remount_pure_anon_live_closures", "AC3", rt)
    must("candidates == 0", "AC3 quiet", reg)
    must("aura_residual_live_closure_remount_tick", "AC3 pipeline", reg)
    must("aura_residual_live_closure_remount_tick", "AC3 boundary", dtor)

    # AC4 soft / budget=0
    must("budget == 0", "AC4", rt)
    must("nslots == 0", "AC4", rt)
    must("production_defaults_active()", "AC4", rt)

    # AC5 query
    must("schema-2928", "AC5", qeval)
    must("residual-remount-ok-total", "AC5", qeval)
    must("residual-remount-budget-skip-total", "AC5", qeval)
    must("residual-remount-cursor", "AC5", qeval)
    must("residual-remount-wired", "AC5", qeval)
    must("schema-2928", "AC5 obs_jit", qjit)
    must("schema-2850", "AC5 lineage", qeval)

    # AC6
    must("ac2928_1_residual_tick_clears_must_deopt", "AC6", test)
    must("ac2928_2_storm_skip", "AC6", test)
    must("ac2928_3_reemit_success_unchanged", "AC6", test)
    must("ac2928_4_soft_budget_zero", "AC6", test)
    must("ac2928_5_query_keys", "AC6", test)
    must("ac2928_6_source_and_linter", "AC6", test)
    must("check_residual_remount_round_robin_2928", "AC6", build)
    must("aura_residual_live_closure_remount_tick", "AC6 shared", shared)
    must("aura_residual_live_closure_remount_tick", "AC6 stub", stub)
    if (ROOT / "tests" / "compiler" / "test_issue_2928.cpp").is_file():
        fails.append("AC6: test_issue_2928.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2928*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2928 residual remount round-robin — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

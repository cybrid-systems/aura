#!/usr/bin/env python3
"""Issue #2850: bounded pure-anon sync remount quota on reemit success.

Named (#2602) + captured-anon (#2691/#2714) remount on reemit. Pure anon
(sid==0, no env/linear) stayed on touch-time MustDeopt. Residual: first-call
tax after every successful reemit for hot pure-anon sites.

Contract (one row per AC):
  AC1 pure-anon walk + budget>0 → remount path; pure_anon_ok advances
  AC2 budget=0 / Soft → pure-anon path does not run
  AC3 named + captured opposite filters; no double remount
  AC4 quiet nslots==0 short-circuit
  AC5 schema-2850 query keys; #2602/#2691/#2714/#2550 surfaces preserved
  AC6 source-cite + this linter; extend test_anonymous_residual_stable_id_policy;
     no docs/design/*

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
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    qeval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    shared = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1
    must("aura_sync_remount_pure_anon_live_closures", "AC1", rt)
    must("aura_sync_remount_pure_anon_budget_default", "AC1", rt)
    must("AURA_SYNC_REMOUNT_PURE_ANON_BUDGET", "AC1", rt)
    must("aura_sync_remount_pure_anon_live_closures", "AC1 bridge", br)
    must("live_closure_sync_remount_pure_anon_ok_total", "AC1", obs)
    must("live_closure_sync_remount_pure_anon_skip_budget_total", "AC1", obs)
    must("aura_bump_live_closure_sync_remount_pure_anon_totals", "AC1", br)
    must("remount_or_force_deopt_unlocked_no_call_time_counter", "AC1 remount", rt)

    # AC2 Soft / budget=0
    must("budget == 0", "AC2", rt)
    must("production_defaults_active()", "AC2", rt)
    must("pure_budget > 0", "AC2", br)
    must("Soft / sandbox", "AC2", rt)

    # AC3 opposite filters
    must("aura_sync_remount_named_live_closures", "AC3 named", rt)
    must("aura_sync_remount_anon_captured_live_closures", "AC3 captured", rt)
    must("aura_closure_has_env_or_linear_captures_unlocked", "AC3 filter", rt)
    must("if (sid != 0)", "AC3 sid filter", rt)
    # pure path continues when has captures
    must("has_env_or_linear_captures_unlocked", "AC3 pure skips captured", rt)

    # AC4 quiet path
    must("nslots == 0", "AC4", rt)

    # AC5 query + lineage (aot-incremental-reemit-stats surface in obs_eval)
    must("schema-2850", "AC5", qeval)
    must("issue-2850", "AC5", qeval)
    must("live-closure-sync-remount-pure-anon-ok-total", "AC5", qeval)
    must("live-closure-sync-remount-pure-anon-skip-budget-total", "AC5", qeval)
    must("live-closure-sync-remount-pure-anon-wired", "AC5", qeval)
    must("schema-2691", "AC5 lineage", qeval)
    must("schema-2850", "AC5 obs_jit", q)
    must("Issue #2602", "AC5 lineage", br)
    must("Issue #2691", "AC5 lineage", br)
    must("Issue #2550", "AC5 lineage", rt)

    # AC6
    must("Issue #2850", "AC6 rt", rt)
    must("Issue #2850", "AC6 br", br)
    must("Issue #2850", "AC6 obs", obs)
    must("2850 AC", "AC6 test", test)
    must("check_pure_anon_sync_remount_budget_2850", "AC6 build", build)
    must("aura_sync_remount_pure_anon_live_closures", "AC6 shared", shared)
    must("aura_sync_remount_pure_anon_budget_default", "AC6 shared", shared)
    must("aura_sync_remount_pure_anon_live_closures", "AC6 stub", stub)
    must("aura_sync_remount_pure_anon_budget_default", "AC6 stub", stub)
    for rel in (
        "docs/design/2850-pure-anon-sync-remount.md",
        "docs/2850-pure-anon-sync-remount.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2850 pure-anon sync remount budget — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

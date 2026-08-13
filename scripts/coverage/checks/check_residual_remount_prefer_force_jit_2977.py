#!/usr/bin/env python3
"""Issue #2977: residual remount prefer force_jit + last_success coverage.

Binds #2928 residual walk to #2895/#2949 coverage masks. Production
prefers closures whose stable_func_id bit (sid % 64) intersects
force_jit_regions_mask | last_reemit_success_region_mask inside budget B.
Soft / mask idle / budget=0 stay #2928 (no prefer path).

Contract (one row per AC):
  AC1 production + multi-bit force_jit prefers demoted sid first
  AC2 Soft / mask idle / budget=0 → no prefer path
  AC3 named/captured/pure remount unchanged; no double remount
  AC4 global cursor still advances (no starvation)
  AC5 additive schema-2977; #2928/#2895/#2949 preserved
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
    hh = _read("src/compiler/hot_update_registry.hh")
    obs = _read("src/compiler/observability_metrics.h")
    qeval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    shared = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")
    mutate = _read("src/compiler/evaluator_primitives_mutate.cpp")

    # AC1
    must("Issue #2977", "AC1", rt)
    must("residual_closure_sid_region_bits_unlocked", "AC1", rt)
    must("prefer_mask", "AC1", rt)
    must("aura_hot_update_force_jit_regions_mask", "AC1", rt)
    must("aura_hot_update_last_reemit_success_region_mask", "AC1", rt)
    must("residual_remount_prefer_force_jit_total", "AC1", obs)
    must("residual_remount_prefer_hit_total", "AC1", obs)
    must("ac2977_1_prefer_demoted_region", "AC1", test)

    # AC2
    must("production_defaults_active()", "AC2", rt)
    must("budget == 0", "AC2", rt)
    must("ac2977_2_soft_idle_zero_cost", "AC2", test)

    # AC3
    must("aura_sync_remount_named_live_closures", "AC3", rt)
    must("aura_sync_remount_anon_captured_live_closures", "AC3", rt)
    must("aura_sync_remount_pure_anon_live_closures", "AC3", rt)
    must("no double remount", "AC3", rt)
    must("candidates == 0", "AC3", reg)
    must("ac2977_3_reemit_success_no_double", "AC3", test)

    # AC4
    must("still advance", "AC4", rt)
    must("ac2977_4_cursor_no_starvation", "AC4", test)
    must("g_residual_remount_cursor", "AC4", rt)

    # AC5
    must("schema-2977", "AC5", qeval)
    must("residual-remount-prefer-force-jit-total", "AC5", qeval)
    must("residual-remount-prefer-hit-total", "AC5", qeval)
    must("residual-remount-prefer-wired", "AC5", qeval)
    must("schema-2977", "AC5 obs_jit", qjit)
    must("schema-2928", "AC5 lineage", qeval)
    must("schema-2895", "AC5 #2895", mutate)
    must("schema-2949", "AC5 #2949", mutate)
    must("ac2977_5_query_keys", "AC5", test)

    # AC6
    must("ac2977_1_prefer_demoted_region", "AC6", test)
    must("ac2977_2_soft_idle_zero_cost", "AC6", test)
    must("ac2977_3_reemit_success_no_double", "AC6", test)
    must("ac2977_4_cursor_no_starvation", "AC6", test)
    must("ac2977_5_query_keys", "AC6", test)
    must("ac2977_6_source_and_linter", "AC6", test)
    must("check_residual_remount_prefer_force_jit_2977", "AC6", build)
    must("Issue #2977", "AC6 registry", reg)
    must("Issue #2977", "AC6 header", hh)
    must("aura_bump_residual_remount_prefer_totals", "AC6", br)
    must("aura_residual_remount_prefer_force_jit_total_v_read", "AC6 shared", shared)
    must("aura_hot_update_force_jit_regions_mask", "AC6 stub", stub)
    if (ROOT / "tests" / "compiler" / "test_issue_2977.cpp").is_file():
        fails.append("AC6: test_issue_2977.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2977*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2977 residual remount prefer force_jit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

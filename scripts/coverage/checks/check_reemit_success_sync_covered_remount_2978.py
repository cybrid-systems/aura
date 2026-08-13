#!/usr/bin/env python3
"""Issue #2978: reemit-success sync covered-named remount.

On production reemit pipeline success with non-zero last_reemit_success
coverage, synchronously remount named (sid!=0) live closures whose sid
bit intersects the coverage mask. Cap N (default 64 production / 0 Soft).
Overflow falls through to residual. Anonymous stay residual / #2950.

Contract (one row per AC):
  AC1 production + covered reemit remounts named in R; MustDeopt cleared
  AC2 Soft / mask==0 / cap==0 → no sync covered walk
  AC3 anon / pure-anon filters unchanged; residual + #2950 own sid==0
  AC4 cap hit does not drop named forever (residual still rotates)
  AC5 additive schema-2978; remount / coverage surfaces preserved
  AC6 source-cite + this linter; extend residual + force-jit suites;
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
    ftest = _read("tests/compiler/test_force_jit_repromote.cpp")
    build = _read("build.py")
    mutate = _read("src/compiler/evaluator_primitives_mutate.cpp")

    # AC1
    must("Issue #2978", "AC1", rt)
    must("aura_sync_remount_covered_named_live_closures", "AC1", rt)
    must("last_reemit_success_region_mask", "AC1", reg)
    must("aura_sync_remount_covered_named_live_closures", "AC1 pipeline", reg)
    must("reemit_success_sync_covered_remount_ok_total", "AC1", obs)
    must("ac2978_1_sync_covered_named", "AC1", test)

    # AC2
    must("mask == 0 || cap == 0", "AC2", rt)
    must("production_defaults_active", "AC2", rt)
    must("aura_production_defaults_active_probe", "AC2", reg)
    must("ac2978_2_soft_mask_idle", "AC2", test)

    # AC3
    must("sid == 0", "AC3", rt)
    must("#2950", "AC3", rt)
    must("aura_sync_remount_named_live_closures", "AC3", rt)
    must("aura_sync_remount_anon_captured_live_closures", "AC3", rt)
    must("ac2978_3_anon_filters", "AC3", test)

    # AC4
    must("leftover", "AC4", rt)
    must("aura_residual_live_closure_remount_tick", "AC4", rt)
    must("reemit_success_sync_covered_remount_cap_hit_total", "AC4", obs)
    must("ac2978_4_cap_overflow_residual", "AC4", test)

    # AC5
    must("schema-2978", "AC5", qeval)
    must("reemit-success-sync-covered-remount-ok-total", "AC5", qeval)
    must("reemit-success-sync-covered-remount-fail-total", "AC5", qeval)
    must("reemit-success-sync-covered-remount-cap-hit-total", "AC5", qeval)
    must("reemit-success-sync-covered-remount-wired", "AC5", qeval)
    must("schema-2978", "AC5 obs_jit", qjit)
    must("schema-2928", "AC5 lineage", qeval)
    must("schema-2977", "AC5 lineage 2977", qeval)
    must("schema-2895", "AC5 coverage", mutate)
    must("schema-2949", "AC5 2949", mutate)
    must("ac2978_5_query_keys", "AC5", test)

    # AC6
    must("ac2978_1_sync_covered_named", "AC6", test)
    must("ac2978_2_soft_mask_idle", "AC6", test)
    must("ac2978_3_anon_filters", "AC6", test)
    must("ac2978_4_cap_overflow_residual", "AC6", test)
    must("ac2978_5_query_keys", "AC6", test)
    must("ac2978_6_source_and_linter", "AC6", test)
    must("check_reemit_success_sync_covered_remount_2978", "AC6", build)
    must("Issue #2978", "AC6 registry", reg)
    must("Issue #2978", "AC6 header", hh)
    must("aura_bump_reemit_success_sync_covered_remount_totals", "AC6", br)
    must("aura_sync_remount_covered_named_live_closures", "AC6 shared", shared)
    must("aura_sync_remount_covered_named_live_closures", "AC6 stub", stub)
    must("2978", "AC6 force-jit suite", ftest)
    if (ROOT / "tests" / "compiler" / "test_issue_2978.cpp").is_file():
        fails.append("AC6: test_issue_2978.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2978*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2978 reemit-success sync covered remount — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

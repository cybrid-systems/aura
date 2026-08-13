#!/usr/bin/env python3
"""Issue #2980: merge event-driven Soft epoch-invariant walk with residual remount.

On epoch-bump / reemit-success under Soft + production_defaults, one
heal pass (a) runs the existing #2668 walk and (b) advances #2928
residual remount when budget > 0. Quiet / Hard / off unchanged.

Contract (one row per AC):
  AC1 Soft production + stale slot + residual + bump → same-edge heal
  AC2 Quiet Soft / budget=0 → no extra remount
  AC3 Hard / epoch_invariant off → no residual merge
  AC4 #2928 / #2668 / #2640 standalone paths preserved
  AC5 Additive schema-2980; #2668/#2928/#2977/#2978 preserved
  AC6 Source-cite + extend residual + epoch-invariant suites;
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

    br = _read("src/compiler/aura_jit_bridge.cpp")
    hh = _read("src/compiler/aura_jit_bridge.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    qeval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qjit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    residual = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    epoch = _read("tests/compiler/test_epoch_invariant_walk.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2980", "AC1", br)
    must("aura_residual_live_closure_remount_tick", "AC1", br)
    must("g_epoch_residual_merged_heal_total", "AC1", br)
    must("aura_event_driven_epoch_invariant_walk_if_due", "AC1", br)
    must("commit_func_table_swap", "AC1", br)
    must("aura_aot_bump_func_table_epoch", "AC1", br)
    must("ac2980_1_merged_heal_same_edge", "AC1", residual)

    # AC2
    must("one relaxed load", "AC2", br)
    must("residual_b > 0", "AC2", br)
    must("ac2980_2_quiet_zero_extra", "AC2", residual)

    # AC3
    must("aura_epoch_invariant_mode() != 1", "AC3", br)
    must("production_defaults_active()", "AC3", br)
    must("ac2980_3_hard_no_merge", "AC3", residual)

    # AC4
    must("aura_residual_live_closure_remount_tick", "AC4", rt)
    must("aura_periodic_epoch_invariant_walk_if_due", "AC4", br)
    must("aura_2693_soft_fuse_record", "AC4", br)
    must("aura_residual_live_closure_remount_tick", "AC4 Boundary", dtor)
    must("ac2980_4_standalone_preserved", "AC4", residual)

    # AC5
    must("schema-2980", "AC5", qeval)
    must("issue-2980", "AC5", qeval)
    must("epoch-residual-merged-heal-total", "AC5", qeval)
    must("epoch-residual-merged-heal-wired", "AC5", qeval)
    must("schema-2668", "AC5 lineage", qeval)
    must("schema-2928", "AC5 residual", qeval)
    must("schema-2980", "AC5 obs_jit", qjit)
    must("ac2980_5_query_keys", "AC5", residual)

    # AC6
    must("Issue #2980", "AC6 header", hh)
    must("aura_epoch_residual_merged_heal_total_v_read", "AC6 header", hh)
    must("aura_epoch_residual_merged_heal_total_v_read", "AC6 stub", stub)
    must("ac2980_1_event_walk_merges_residual", "AC6 epoch", epoch)
    must("ac2980_6_source_and_linter", "AC6 residual", residual)
    must("check_epoch_residual_merged_heal_2980", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2980.cpp").is_file():
        fails.append("AC6: test_issue_2980.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2980*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2980 event-walk + residual remount merged heal — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

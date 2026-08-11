#!/usr/bin/env python3
"""Issue #2890: cross-fiber steal residual for PanicCheckpoint + GC defer.

Contract (one row per AC):
  AC1  steal-complete force-clears the PREVIOUS host evaluator's live
       PanicCheckpoint under production (prev_eval_id walked, not only the
       current scheduler eval); g_residual_defer_steal_checkpoint_cleared_total
  AC2  Soft / no checkpoint / opaque prev id → zero extra work (block gated
       on production_force + checkpoint presence)
  AC3  same-eval continuity → transfer counter, no double-clear (#1727 /
       #2667 paths preserved)
  AC4  additive counters + query keys + schema-2890; #2846/#2667/#2710
       surfaces preserved
  AC5  source-cite + tests in existing src/-aligned steal-residual suite
       (tests/serve/test_residual_defer_steal_hard_and.cpp, #81967);
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/gc_hooks.h",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "tests/serve/test_residual_defer_steal_hard_and.cpp",
    "scripts/coverage/checks/check_steal_checkpoint_residual_2890.py",
]


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

    gc = _read("src/core/gc_hooks.h")
    rt = _read("src/compiler/evaluator_fiber_mutation.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    obs_eval = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/serve/test_residual_defer_steal_hard_and.cpp")
    build = _read("build.py")

    # ── AC1: prev-host force-clear on steal-complete ──
    must("Issue #2890", "AC1", rt)
    must("g_residual_defer_steal_checkpoint_cleared_total", "AC1", rt)
    must("static_cast<Evaluator*>(prev_eval_id)", "AC1", rt)
    must("prev_ev->has_panic_checkpoint()", "AC1", rt)
    must("prev_ev->clear_panic_checkpoint()", "AC1", rt)
    must("production_residual_policy_locked", "AC1", rt)
    must("g_residual_defer_steal_checkpoint_cleared_total", "AC1", gc)
    must("kResidualDeferStealCheckpointIssue = 2890", "AC1", gc)

    # ── AC2: Soft / no checkpoint → zero extra work ──
    must("if (production_force && prev_addr > 0x100000ull)", "AC2", rt)
    must("prev_ev->has_panic_checkpoint()", "AC2", rt)

    # ── AC3: same-eval transfer, no double-clear ──
    must("g_residual_defer_steal_checkpoint_transfer_total", "AC3", rt)
    must("prev_ev == cur_ev && prev_ev->has_panic_checkpoint()", "AC3", rt)
    must("g_residual_defer_steal_checkpoint_transfer_total", "AC3", gc)

    # ── AC4: additive query keys + schema; prior preserved ──
    must("residual-defer-steal-checkpoint-cleared-total", "AC4", obs)
    must("residual-defer-steal-checkpoint-transfer-total", "AC4", obs)
    must("residual-defer-steal-checkpoint-wired", "AC4", obs)
    must("schema-2890", "AC4", obs)
    must("issue-2890", "AC4", obs)
    must("schema-2667", "AC4", obs)
    must("schema-2710", "AC4", obs)
    must("residual-defer-after-exit-total", "AC4", obs_eval)

    # ── AC5: source-cite + suite + build.py gate ──
    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC5: missing file {rel}")
            continue
        if "Issue #2890" not in content:
            fails.append(f"AC5: {rel} does not cite Issue #2890")
    must("ac2890_1_prev_host_clear", "AC5", test)
    must("ac2890_2_soft_zero_extra_work", "AC5", test)
    must("ac2890_3_no_double_clear", "AC5", test)
    must("ac2890_4_query_and_surface", "AC5", test)
    must("ac2890_5_linter_and_no_design", "AC5", test)
    must("check_steal_checkpoint_residual_2890", "AC5", build)
    design_docs = sorted((ROOT / "docs" / "design").glob("2890-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC5: docs/design/2890-* present ({[p.name for p in design_docs]})")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_steal_checkpoint_residual_2890: {len(fails)} failure(s)")
        return 1

    print("check_steal_checkpoint_residual_2890: OK (AC1-AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

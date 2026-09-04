#!/usr/bin/env python3
"""Issue #3478: pure-anon budget skip stamps MustDeopt (skip ≠ overflow).

Overflow (#3024/#3323) is fail-closed. In-budget skip was not: skipped
cids waited for residual tick / bg drain. Production skip arm now sets
g_closure_must_deopt[cid]=1 (optional epoch poison) before return.
Soft / budget=0: no extra MustDeopt stores. Overflow helper unchanged.

Contract:
  AC1  skip arm stamps MustDeopt under production_defaults_active
  AC2  next aura_closure_call on skipped cid leaves native
  AC3  skip arm does not call overflow helper / bump overflow counters
  AC4  named + captured walks unchanged
  AC5  Soft / budget=0: no extra MustDeopt stores
  AC6  extend test_anonymous_residual_stable_id_policy; linter AFTER #3323

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    start = rt.find("void aura_sync_remount_pure_anon_live_closures")
    end = rt.find("Issue #2950: enqueue budget-exhausted pure-anon after table unlock")
    if start < 0 or end < 0 or end <= start:
        fails.append("AC6: pure-anon walk window missing")
        win = ""
    else:
        win = rt[start:end]
        must("Issue #3478", "AC6 cite", win)
        must("if (used >= budget)", "AC1 skip arm", win)
        skip = win.find("if (used >= budget)")
        remount = win.find("remount_or_force_deopt_unlocked_no_call_time_counter")
        if skip < 0 or remount < 0 or remount < skip:
            fails.append("AC1: skip arm must dominate remount")
            arm = ""
        else:
            arm = win[skip:remount]
            must("g_closure_must_deopt[cid] = 1", "AC1 MustDeopt", arm)
            must("production_defaults_active()", "AC5 production gate", arm)
            must_not("pure_anon_bg_overflow_force_leave_native", "AC3 no overflow helper", arm)
            must_not("overflow_must_deopt", "AC3 no overflow counter", arm)
            must_not("aura_jit_batch_deopt_for", "AC1 no batch_deopt unnamed", arm)

    must("ac3478_1_skip_stamps_must_deopt", "AC1 test", test)
    must("3478 AC1: skipped cids have must_deopt==1 after walk", "AC1 assert", test)
    must("ac3478_2_skipped_call_leaves_native", "AC2 test", test)
    must("3478 AC2: skipped call leaves native", "AC2 assert", test)
    must("ac3478_3_skip_not_overflow", "AC3 test", test)
    must("aura_sync_remount_named_live_closures", "AC4 named", rt)
    must("aura_sync_remount_anon_captured_live_closures", "AC4 captured", rt)
    must("ac3478_4_named_captured_unchanged", "AC4 test", test)
    must("ac3478_5_soft_no_extra", "AC5 test", test)
    must("budget == 0", "AC5 budget=0", rt)
    must("pure_budget = aura_sync_remount_pure_anon_budget_base()", "AC6 storm shrink", br)

    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos != -1:
        swin = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in swin:
            fails.append("AC4: steal-complete drains pure-anon (forbidden #2715)")
    else:
        fails.append("AC4: steal-complete site not found")

    must("pure_anon_bg_overflow_force_leave_native", "AC3 overflow helper kept", rt)
    must_not("schema-3478", "AC6 no query key", rt)
    must_not("g_3478_", "AC6 no g_3478_*", rt)

    must("check_pure_anon_budget_skip_must_deopt_3478", "AC6 build.py", build)
    prev = build.find("check_pure_anon_overflow_dispatch_race_3323")
    ours = build.find("check_pure_anon_budget_skip_must_deopt_3478")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3323")

    if (ROOT / "tests" / "compiler" / "test_issue_3478.cpp").is_file():
        fails.append("AC6: test_issue_3478.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3478-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3478 pure_anon_budget_skip_must_deopt:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3478 pure_anon_budget_skip_must_deopt: skip stamps MustDeopt; overflow unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())

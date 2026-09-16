#!/usr/bin/env python3
"""Issue #3851: pure-anon budget-skip arms sticky overflow fence.

#3478 stamped MustDeopt + poisoned bridge_epoch on production budget-skip
but did not arm g_closure_pure_anon_overflow_armed. MustDeopt consumption
cleared the flag; second call escaped #3410 (both-nonzero only) via
soft-migrate and could dispatch pre-reemit native. Align skip with the
#3323 overflow sticky fence SSOT. Soft / budget=0: enqueue-only.

Contract (one row per AC):
  AC1  production budget-skip arms sticky + invalidate (not overflow helper)
  AC2  Soft skip path does not arm sticky; heal clears sticky (SSOT)
  AC3  soft-migrate refuses cap_bridge==0 under production tracking;
       post soft-migrate re-checks aura_is_jit_closure_fresh
  AC4  extends test_anonymous_residual_stable_id_policy; linter AFTER
       #3478; grandfather + manifest + build.py; no invent / docs/design;
       no schema-3851 / g_3851_* production counters

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RT = "src/compiler/aura_jit_runtime.cpp"
TEST = "tests/compiler/test_anonymous_residual_stable_id_policy.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3851.json"
LINTER = "check_pure_anon_budget_skip_sticky_3851"
PREV = "check_pure_anon_budget_skip_must_deopt_3478"


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
            fails.append(f"{label}: forbidden {n!r} present")

    rt = _read(RT)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    start = rt.find("void aura_sync_remount_pure_anon_live_closures")
    end = rt.find("Issue #2950: enqueue budget-exhausted pure-anon after table unlock")
    if start < 0 or end < 0 or end <= start:
        fails.append("AC1: pure-anon walk window missing")
        win = ""
    else:
        win = rt[start:end]
        must("Issue #3851", "AC1 cite", win)
        must("if (used >= budget)", "AC1 skip arm", win)
        skip = win.find("if (used >= budget)")
        remount = win.find("remount_or_force_deopt_unlocked_no_call_time_counter")
        if skip < 0 or remount < 0 or remount < skip:
            fails.append("AC1: skip arm must dominate remount")
            arm = ""
        else:
            arm = win[skip:remount]
            must("g_closure_must_deopt[cid] = 1", "AC1 MustDeopt retained", arm)
            must("g_closure_bridge_epochs[cid] = 0", "AC1 bridge poison", arm)
            must("g_closure_pure_anon_overflow_armed[cid] = 1", "AC1 sticky arm", arm)
            must("invalidate_closure_cache_for", "AC1 cache invalidate", arm)
            must("production_defaults_active()", "AC1 production gate", arm)
            must_not("pure_anon_bg_overflow_force_leave_native", "AC1 no overflow helper", arm)
            must_not("overflow_must_deopt", "AC1 no overflow counter", arm)

    # Soft: sticky arm only inside production_defaults_active block.
    must("Soft: enqueue", "AC2 Soft enqueue cite", rt)
    must(
        "remount heal with dual-fresh green disarms the",
        "AC2 heal clears sticky SSOT",
        rt,
    )
    must("g_closure_pure_anon_overflow_armed[cid] = 0", "AC2 sticky clear sites", rt)

    must("production + bridge tracking treats cap_bridge==0 as", "AC3 epoch0 cite", rt)
    must("(bridge_tracking && cap_bridge == 0)", "AC3 epoch0 refuse", rt)
    must("re-check dual-fresh after soft-migrate", "AC3 post-migrate cite", rt)
    must(
        "if (!aura_is_jit_closure_fresh(post_bridge, post_defuse, post_table))",
        "AC3 post-migrate fresh",
        rt,
    )

    must("ac3851_1_two_call_leave_native_after_budget_skip", "AC4 test AC1", test)
    must("3851 AC1: second call leaves native", "AC4 assert two-call", test)
    must("ac3851_2_heal_clears_sticky_soft_no_arm", "AC4 test AC2", test)
    must("ac3851_3_soft_migrate_epoch0_and_post_fresh", "AC4 test AC3", test)
    must("ac3851_4_source_and_linter", "AC4 test AC4", test)

    must(LINTER, "AC4 build registration", build)
    must("Issue #3851", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC4 grandfather path", gf)
    must('"issue": 3851', "AC4 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC4: {MANIFEST} missing")

    prev = build.find(PREV)
    ours = build.find(LINTER)
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3478")

    must_not("schema-3851", "AC4 no query key", rt)
    must_not("g_3851_", "AC4 no g_3851_* production", rt)

    for rel in (
        "tests/compiler/test_issue_3851.cpp",
        "tests/issues/test_issue_3851.cpp",
        "docs/design/3851-pure-anon-budget-skip-sticky.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3851*"):
            fails.append(f"AC4: docs/design/{p.name} exists")

    # Overflow sticky SSOT still present (do not claim missing).
    must("pure_anon_bg_overflow_force_leave_native", "AC1 overflow helper kept", rt)
    must("g_closure_pure_anon_overflow_armed[cid] = 1", "AC1 overflow sticky kept", rt)

    if fails:
        print("FAIL #3851 pure_anon_budget_skip_sticky:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(
        "OK #3851 pure_anon_budget_skip_sticky: skip arms sticky; Soft enqueue-only; "
        "heal SSOT; soft-migrate epoch0 + post-fresh"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

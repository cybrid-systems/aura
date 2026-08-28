#!/usr/bin/env python3
"""Issue #3323: pure-anon overflow MustDeopt+poison is visible before native.

Residual after #2714/#2850/#2893/#2950/#3024/#3060: overflow already sets
MustDeopt + poisons bridge_epoch, but a concurrent call prologue that
sampled the pre-poison epoch could still dispatch to old native. Production
publishes a release fence + overflow epoch + cache invalidate, and
aura_closure_call last-looks MustDeopt before dispatch. RenderFastExit
still drains when pending. Soft / budget=0: helper never runs.

Contract (one row per AC):
  AC1  production overflow → MustDeopt + epoch poison + cache drop +
       subsequent call leaves native
  AC2  concurrent call during overflow → no stale native (last-look +
       overflow epoch seqlock)
  AC3  BoundaryExit drain still runs after overflow (incl. render-fast)
  AC4  Soft / budget=0: no fence, no overflow-epoch bump, no drain extra
  AC5  named/captured remount + storm-clear + steal-complete unchanged;
       extends test_anonymous_residual_stable_id_policy (#81967); no
       test_issue_3323.cpp; no docs/design/; no schema-3323 / g_3323_*

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    hur = _read("src/compiler/hot_update_registry.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    must("Issue #3323", "AC1 cite", rt)
    must("pure_anon_bg_overflow_force_leave_native", "AC1 helper", rt)
    must("g_closure_must_deopt[cid] = 1", "AC1 MustDeopt", rt)
    must("g_closure_bridge_epochs[cid] = 0", "AC1 poison", rt)
    must("invalidate_closure_cache_for(closure_id)", "AC1 cache drop", rt)
    must("g_pure_anon_overflow_epoch", "AC1 overflow epoch", rt)
    must("ac3323_1_overflow_no_subsequent_native", "AC1 test", test)

    must("last-look MustDeopt before any native dispatch", "AC2 last-look", rt)
    must("ov_samp", "AC2 seqlock sample", rt)
    must("ac3323_2_concurrent_call_no_stale_native", "AC2 test", test)

    must("MUST still drain when pending", "AC3 render-fast drain", mb)
    must("aura_pure_anon_bg_remount_drain", "AC3 drain call", mb)
    must("ac3323_3_boundary_drain_after_overflow", "AC3 test", test)
    cite = mb.find("Issue #3323: RenderFastExit MUST still drain")
    if cite < 0:
        fails.append("AC3: #3323 drain cite missing")
    elif "aura_pure_anon_bg_remount_drain" not in mb[max(0, cite - 200) : cite + 200]:
        fails.append("AC3: #3323 drain cite/call not on outermost success tail")

    must("Soft never calls this helper", "AC4 soft helper", rt)
    must("ov_samp stays 0", "AC4 soft epoch", rt)
    must("Soft / budget=0: max_n==0, no drain", "AC4 soft drain", mb)
    must("ac3323_4_soft_zero_extra", "AC4 test", test)

    must("aura_sync_remount_named_live_closures", "AC5 named", rt)
    must("aura_sync_remount_anon_captured_live_closures", "AC5 captured", rt)
    must("maybe_storm_clear_health_pass", "AC5 storm-clear", hur)
    must("check_pure_anon_overflow_dispatch_race_3323", "AC5 build.py", build)
    must("ac3323_5_source_and_linter", "AC5 test", test)
    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos != -1:
        win = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in win:
            fails.append("AC5: steal-complete drains pure-anon (forbidden #2715)")
    else:
        fails.append("AC5: steal-complete site not found")
    prev = build.find("check_pure_anon_no_boundary_force_leave_3277")
    ours = build.find("check_pure_anon_overflow_dispatch_race_3323")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3277")
    if "schema-3323" in rt or "schema-3323" in mb:
        fails.append("AC5: new schema-3323 query key")
    if "g_3323_" in rt or "g_3323_" in mb:
        fails.append("AC5: new g_3323_* counter in production")
    if _read("tests/compiler/test_issue_3323.cpp"):
        fails.append("AC5: test_issue_3323.cpp present (forbidden #81967)")
    if _read("docs/design/3323-pure-anon-overflow-dispatch.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3323 pure_anon_overflow_dispatch_race:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3323 pure_anon_overflow_dispatch_race: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3277: close the pure-anon no-boundary first-call hole.

Residual of #2850/#2950/#3060: under production high-frequency self-mod
WITHOUT an outermost MutationBoundary success-exit, budget-skipped
pure-anon (sid==0 && !captures) slots stayed on touch-time MustDeopt only
until BoundaryExit / residual tick heals — the first post-reemit native
call could still pay MustDeopt jitter (non-overflow past-budget was not
aggressively leave-native until drain or call). #3277 closes the window
with ONE additional safe site: the reemit-success pure-anon walk itself.

Contract (one row per AC):
  AC1  production + walk skip > 0 → force leave-native on the oldest
       pending/skipped slots (reuse #3060 pressure helper + #3024
       overflow semantics; helper clamps to batch size)
  AC2  Soft / !production skip → counter-only, no force-leave
  AC3  budget=0 → zero walk / no force (Soft zero-cost unchanged)
  AC4  storm throttle shrinks budget but does NOT reopen a permanent
       native hole (walk still force-leaves on skip); steal-complete
       never drains / forces pure-anon (#2715 preserved)
  AC5  source-cite in aura_jit_runtime.cpp + test extends
       test_anonymous_residual_stable_id_policy (#81967); no
       test_issue_3277.cpp; no docs/design/ (#1655); build.py wires
       linter; no new query keys / counters (reuses
       pure_anon_bg_overflow_must_deopt_total)

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
    br = _read("src/compiler/aura_jit_bridge.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    must("Issue #3277", "AC1/AC5 cite", rt)
    must("pure_anon_pressure_force_leave_oldest(skip)", "AC1 walk force-leave", rt)
    must("close the no-boundary first-call hole", "AC1 doc", rt)
    must("production_defaults_active()", "AC1 production gate", rt)
    must("reuse #3060 pressure helper", "AC1 helper reuse", rt)
    must("ac3277_1_prod_walk_skip_force_leave", "AC1 test", test)
    must("ac3277_2_soft_skip_counter_only", "AC2 test", test)
    must("ac3277_3_budget_zero_no_force", "AC3 test", test)
    must("ac3277_4_storm_shrink_and_no_steal_drain", "AC4 test", test)
    must("ac3277_5_source_and_linter", "AC5 test", test)
    must("should_throttle_reemit()", "AC4 storm gate", br)
    must("pure_budget = aura_sync_remount_pure_anon_budget_base()", "AC4 storm shrink", br)
    must("check_pure_anon_no_boundary_force_leave_3277", "AC5 build.py", build)
    # AC4: steal-complete must not drain / force pure-anon (#2715 preserved).
    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos != -1:
        win = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in win:
            fails.append("AC4: steal-complete drains pure-anon bg (forbidden #2715)")
        if "pure_anon_pressure_force_leave_oldest" in win:
            fails.append("AC4: steal-complete force-leaves pure-anon (forbidden #2715)")
    else:
        fails.append("AC4: steal-complete site not found")
    if _read("tests/compiler/test_issue_3277.cpp"):
        fails.append("AC5: test_issue_3277.cpp present (forbidden #81967)")
    if _read("docs/design/3277-pure-anon-no-boundary-force.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3277 pure_anon_no_boundary_force_leave:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3277 pure_anon_no_boundary_force_leave: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

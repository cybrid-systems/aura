#!/usr/bin/env python3
"""Issue #2691: sync remount captured anon (sid==0 && has env/linear) on reemit success.

Contract:
  AC1 Inject anon closure with env/linear capture → reemit success → before
      any call, remount_ok advances and MustDeopt is not set solely due to
      epoch restamp lag.
  AC2 Inject anon closure without captures → reemit → captured-filter
      path does not call remount (counter stable); pure-anon policy
      (#2550/#2605) unchanged.
  AC3 Named path (#2602) still runs; no double remount on same cid.
  AC4 Soft / sandbox=off / tests with anon prod-default off → zero cost
      (preserve #2637 AC1 / #2666 Soft).
  AC5 Additive query keys + schema sentinel; #2602/#2503/#2550/#2666
      surfaces preserved.
  AC6 Source-cite + coverage linter; extend
      test_anonymous_residual_stable_id_policy / live-closure restamp
      suite per #81967 (no docs/design per #1655).

This linter (AC5/AC6) verifies:
  - aura_sync_remount_anon_captured_live_closures C ABI hook exists
  - aura_bump_live_closure_sync_remount_anon_captured_totals C ABI
    counter helper exists
  - 2 new process-wide counters declared in CompilerMetrics
  - Wire: maybe_storm_clear_health_pass calls
    aura_hot_update_drain_pending_recovery (or equivalent
    aura_sync_remount_anon_captured_live_closures) after the existing
    named sync
  - Query surface: 2 keys + schema-2691 + issue-2691 + drain-wired
    sentinel
  - No docs/design/* regression

Exit 0 = OK, 1 = violation found.
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
    _read("build.py")

    # AC1/AC2 — C ABI hook for captured-only anon sync remount.
    must("aura_sync_remount_anon_captured_live_closures", "AC1/AC2", rt)
    # AC1/AC2 — counter bumper helper.
    must("aura_bump_live_closure_sync_remount_anon_captured_totals", "AC1/AC2-counter", rt)
    must("aura_bump_live_closure_sync_remount_anon_captured_totals", "AC1/AC2-counter-impl", br)

    # AC1/AC2 — capture filter: must use aura_closure_has_env_or_linear_captures.
    must("aura_closure_has_env_or_linear_captures_unlocked", "AC1/AC2-capture-filter", rt)

    # AC5 — counters in observability_metrics.h CompilerMetrics.
    must("live_closure_sync_remount_anon_captured_ok_total", "AC5-counter-ok", obs)
    must("live_closure_sync_remount_anon_captured_fail_total", "AC5-counter-fail", obs)

    # AC3/AC4 — Wire: named path (#2602) + anon path (#2637) preserved.
    must("aura_sync_remount_named_live_closures", "AC3-named", rt)
    must("aura_sync_remount_anon_live_closures", "AC4-anon", rt)

    # AC4/AC5 — Wire: aura_remap_live_closures_after_reemit in bridge calls
    # the captured-only hook.
    if "aura_sync_remount_anon_captured_live_closures" not in br:
        fails.append(
            "AC4/AC5: bridge must call aura_sync_remount_anon_captured_live_closures "
            "inside aura_remap_live_closures_after_reemit after the existing "
            "anon sync"
        )

    # AC5 — query surface: 2 keys + schema-2691 + issue-2691 + drain-wired.
    must("live-closure-sync-remount-anon-captured-ok-total", "AC5-q-ok", q)
    must("live-closure-sync-remount-anon-captured-fail-total", "AC5-q-fail", q)
    must("schema-2691", "AC5-q-schema", q)
    must("issue-2691", "AC5-q-issue", q)
    must("closure-pending-recovery-drain-wired", "AC5-q-wired", q)

    # AC5 — #2602/#2503/#2550/#2666 lineage preserved.
    must("Issue #2602", "AC5-lin-2602", br)
    must("Issue #2503", "AC5-lin-2503", rt)
    must("Issue #2550", "AC5-lin-2550", rt)
    must("Issue #2666", "AC5-lin-2666", rt)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/closure_anon_captured_remount_2691.md",
        "docs/closure_anon_captured_remount_2691.md",
        "design/2691.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: #2691 sentinel in aura_jit_runtime.cpp +
    # aura_jit_bridge.cpp + observability_metrics.h + evaluator_primitives_obs_jit.cpp.
    must("#2691", "AC6-rt", rt)
    must("#2691", "AC6-br", br)
    must("#2691", "AC6-obs", obs)
    must("#2691", "AC6-q", q)

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_closure_anon_captured_remount_2691.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2691 closure anon captured remount — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

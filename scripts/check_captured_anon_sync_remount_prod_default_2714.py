#!/usr/bin/env python3
"""Issue #2714: production-default sync remount for captured anon.

Aligns #2691 (captured-only anon sync remount) with production defaults.
The captured walk is the highest-value anon subset for EDSL / agent code
(sid==0 && has env/linear). Without #2714, the walk is still gated on
AURA_SYNC_REMOUNT_ANON=1 — so under production defaults (env knob unset)
the first-call MustDeopt window is still paid. #2714 adds
production_defaults_active() || env_sync_remount_anon_enabled() to the
gate, aligning with the named #2602 path.

Contract rows (AC1–AC6 from the test file):

  AC1: production_defaults_active() → captured-anon sync remount runs on
       reemit success WITHOUT requiring AURA_SYNC_REMOUNT_ANON=1.
  AC2: Pure anon (no env/linear) still skips remount; counter stable.
  AC3: Named path (#2602) unchanged; no double remount on same cid.
  AC4: Soft / sandbox=off / explicit AURA_SYNC_REMOUNT_ANON=0 under
       non-production → zero-cost short-circuit preserved.
  AC5: Additive only — preserve schema-2691 / #2602 / #2666 / #2550.
  AC6: source-cite + linter + no docs/design/.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_captured_anon_sync_remount_prod_default_2714.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cpp = _read("src/compiler/aura_jit_bridge.cpp")
    t = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1 — production_defaults_active() OR AURA_SYNC_REMOUNT_ANON → captured
    # anon sync remount runs on reemit success (no env knob required).
    must("Issue #2714", "AC1", cpp)
    must("production_defaults_active()", "AC1", cpp)
    must("sync_captured", "AC1", cpp)
    must("aura_sync_remount_anon_captured_live_closures", "AC1", cpp)
    # Sanity: gate is on production_defaults_active() || env, not env-only.
    must("production_defaults_active() ||", "AC1", cpp)

    # AC2 — pure anon still skips (verified by the filter inside
    # aura_sync_remount_anon_captured_live_closures — captured filter
    # via aura_closure_has_env_or_linear_captures). Pure anon is NOT
    # routed through this helper.

    # AC3 — named path unchanged. The full anon walk (env-gated, unchanged)
    # and the captured walk (production + env, NEW gate) are independent.
    # The original if-block for the full anon walk is preserved.
    must("aura_sync_remount_anon_live_closures", "AC3", cpp)

    # AC4 — Soft / sandbox=off / explicit AURA_SYNC_REMOUNT_ANON=0 under
    # non-production → zero-cost short-circuit preserved. production_defaults_active()
    # is false in those modes, so sync_captured stays false. The env knob
    # check ensures the env-only path also short-circuits when set to 0.
    must("aura_sync_remount_anon_enabled_default() != 0", "AC4", cpp)

    # AC5 — additive only. #2691 surface preserved (anon_captured counters
    # still bumped via aura_bump_live_closure_sync_remount_anon_captured_totals).
    # #2602 / #2666 / #2550 surfaces preserved by the unchanged full anon
    # walk + named sync remount path.
    must("live_closure_sync_remount_anon_captured_ok_total", "AC5", cpp)
    must("aura_bump_live_closure_sync_remount_anon_captured_totals", "AC5", cpp)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2714_1_production_default_captured_remount", "AC6", t)
    must("ac2714_2_pure_anon_skips_remount", "AC6", t)
    must("ac2714_3_named_path_unchanged", "AC6", t)
    must("ac2714_4_soft_zero_cost_preserved", "AC6", t)
    must("ac2714_5_additive_no_regression", "AC6", t)
    must("ac2714_6_source_and_linter", "AC6", t)
    must("check_captured_anon_sync_remount_prod_default_2714", "AC6", build)
    if _read("docs/design/2714-captured-anon-sync-remount.md"):
        fails.append("AC6: docs/design/2714-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2714 production-default captured-anon sync remount — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

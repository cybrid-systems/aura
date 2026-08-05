#!/usr/bin/env python3
"""Issue #2668: event-driven epoch-invariant walk on table epoch bump (beyond periodic Soft).

Contract (one row per AC):
  AC1 src/compiler/aura_jit_bridge.cpp defines aura_event_driven_epoch_invariant_walk_if_due
     + calls it from commit_func_table_swap + aura_aot_bump_func_table_epoch
     (after notify_epoch_bump). Production + Soft + inject stale →
     behind count drops to 0 without waiting for period.
  AC2 Soft / Off / mode=0 → no event walk on bump (gates respected —
     mode != 1 → skipped_wrong_mode, !production_defaults → skipped_off).
  AC3 shares last_walk_at_ms atomic with periodic path (no double
     physical clear in same window — AC4 in #2640 reused).
  AC4 #2541 / #2640 soft semantics preserved (reuses the same walk
     bodies: aura_aot_invalidate_all_stale_slots_for_eval(nullptr) +
     aura_epoch_invariant_must_deopt_stale_live_closures).
  AC5 src/compiler/evaluator_primitives_obs_eval.cpp exposes additive
     query sentinels: epoch-invariant-event-walks-total +
     epoch-invariant-event-skipped-off-total +
     epoch-invariant-event-skipped-wrong-mode-total +
     epoch-invariant-event-wired + schema-2668 + issue-2668. #2640
     / #2541 / #2366 surfaces preserved (additive — no regression).
  AC6 tests/compiler/test_epoch_invariant_walk.cpp extended with
     #2668 AC1-AC6 source-cite block (per #81967 — no new issue-suffix
     file).
  AC7 build.py wires check_2668_coverage into the gate after
     check_2667_coverage.
  AC8 cross-check: check_epoch_invariant_periodic_coverage + check_stamp_
     resolve_coverage still green (no regression on #2640 / #2366).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_epoch_invariant_walk.cpp")
    build = _read("build.py")

    # AC1 — event-driven walk wired on both bump sites
    must("aura_event_driven_epoch_invariant_walk_if_due", "AC1", br)
    must("Issue #2668", "AC1", br)
    must("commit_func_table_swap", "AC1", br)
    must("aura_aot_bump_func_table_epoch", "AC1", br)
    # Counter declarations
    must("g_epoch_invariant_event_walks_total{0}", "AC1", br)
    must("g_epoch_invariant_event_skipped_off_total{0}", "AC1", br)
    must("g_epoch_invariant_event_skipped_wrong_mode_total{0}", "AC1", br)
    # Gating (mirror #2640)
    must("production_defaults_active()", "AC2", br)
    must("aura_epoch_invariant_mode() != 1", "AC2", br)
    # AC3 — shared last_walk_at_ms
    must("g_epoch_invariant_periodic_last_walk_at_ms.store", "AC3", br)
    # AC4 — #2541 / #2640 soft semantics preserved (same walk bodies)
    must("aura_aot_invalidate_all_stale_slots_for_eval(nullptr)", "AC4", br)
    must("aura_epoch_invariant_must_deopt_stale_live_closures", "AC4", br)
    # Prior surfaces preserved (regression)
    must("g_epoch_invariant_periodic_walks_total{0}", "AC4", br)
    must("aura_periodic_epoch_invariant_walk_if_due", "AC4", br)

    # AC5 — additive query sentinels in obs_eval.cpp
    must("epoch-invariant-event-walks-total", "AC5", obs)
    must("epoch-invariant-event-skipped-off-total", "AC5", obs)
    must("epoch-invariant-event-skipped-wrong-mode-total", "AC5", obs)
    must("epoch-invariant-event-wired", "AC5", obs)
    must("schema-2668", "AC5", obs)
    must("issue-2668", "AC5", obs)
    # Prior surfaces preserved
    must("epoch-invariant-periodic-walks-total", "AC5", obs)
    must("schema-2640", "AC5", obs)
    must("epoch-invariant-wired", "AC5", obs)

    # AC6 — test file extension
    must("ac2668_1_event_driven_walk_wired", "AC6", test)
    must("ac2668_2_query_keys_added", "AC6", test)
    must("ac2668_3_build_linter_wired", "AC6", test)
    must("Issue #2668", "AC6", test)

    # AC7 — build.py wires the linter
    must("check_2668_coverage", "AC7", build)

    # Cross-check: check_epoch_invariant_periodic_coverage still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_epoch_invariant_periodic_coverage.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_epoch_invariant_periodic_coverage regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: check_stamp_resolve_coverage --strict still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"),
            "--strict",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_stamp_resolve_coverage --strict regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2668 event-driven epoch-invariant walk on table epoch bump — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2722: static contract for RELEASE chaos SOAK hard deploy gate.

The chaos harness introduced in #2679 + the PR chaos hard-fail gate
(#2554) cover the critical interleavings, but neither is REQUIRED
for tag / release artifacts — the chaos SOAK was "optional / best-effort"
and could slip through silent corruption or resume-ticket races.

#2722 makes the FULL chaos SOAK a REQUIRED release deploy gate
under production_defaults_active + Hard fail-closed. This linter
validates the contract:

  AC1: cmd_chaos_soak_hard_gate_2722 exists in build.py (RELEASE hard
       gate, distinct from PR smoke + nightly paths).
  AC2: hard-fail env matrix forces production_defaults_active + Hard
       (AURA_PRODUCTION_CONCURRENCY_GATE=1 + AURA_CHAOS_FULL=1 +
       AURA_CHAOS_SOAK=1 + AURA_CHAOS_SOAK_HARD_GATE=1; Soft steal
       AURA_STEAL_SNAPSHOT_SOFT explicitly popped).
  AC3: production envelope documented in function docstring
       (workers=8, fibers=256, duration=300s, seed=1, mb_starve_max=0).
  AC4: required for any tag / release candidate — wired in
       .github/workflows/release.yml as a required step BEFORE the
       release-asset upload step.
  AC5: Soft mode (AURA_STEAL_SNAPSHOT_SOFT=1) explicitly non-gating —
       env.pop'd under the hard gate matrix; available for local
       iteration via cmd_chaos_mutate_steal_gc_mailbox_coverage / PR
       gate paths but NOT under cmd_chaos_soak_hard_gate_2722.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import re
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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    def must_match(pattern: str, label: str, hay: str) -> None:
        if not re.search(pattern, hay):
            fails.append(f"{label}: pattern {pattern!r} not found")

    build = _read("build.py")
    release = _read(".github/workflows/release.yml")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")

    # ============================================================
    # AC1: cmd_chaos_soak_hard_gate_2722 exists in build.py
    # ============================================================
    must("def cmd_chaos_soak_hard_gate_2722(", "AC1", build)
    must("cmd_chaos_soak_hard_gate_2722_coverage", "AC1", build)
    # Issue stamp + Issue: #2722 attribution in the docstring.
    must("Issue #2722", "AC1", build)
    # Command-table registration (so ./build.py chaos-soak-hard-gate-2722
    # + ./build.py chaos-soak-hard-gate-2722-coverage work).
    must('"chaos-soak-hard-gate-2722": cmd_chaos_soak_hard_gate_2722,', "AC1", build)
    must(
        '"chaos-soak-hard-gate-2722-coverage": cmd_chaos_soak_hard_gate_2722_coverage,',
        "AC1",
        build,
    )
    # Wired into main gate command chain (coverage-only for fast pre-push
    # feedback; the full SOAK runs in release.yml per AC4).
    must_match(
        r"or\s+cmd_chaos_soak_hard_gate_2722_coverage\(\)",
        "AC1",
        build,
    )
    # Full function registered in command table for release.yml invocation.
    must('"chaos-soak-hard-gate-2722": cmd_chaos_soak_hard_gate_2722,', "AC1", build)

    # ============================================================
    # AC2: hard-fail env matrix forces production_defaults_active + Hard
    # ============================================================
    # Hard gate env matrix (distinct env per #2554 PR gate so they're
    # independently gated — PR CI vs nightly vs RELEASE).
    must('env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"', "AC2", build)
    must('env["AURA_LOCK_ORDER_CANARY"] = "1"', "AC2", build)
    must('env["AURA_CHAOS_FULL"] = "1"', "AC2", build)
    must('env["AURA_CHAOS_SOAK"] = "1"', "AC2", build)
    must('env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"', "AC2", build)
    # Soft steal FORBIDDEN under hard gate (production_defaults_active + Hard).
    must_match(
        r'env\.pop\("AURA_STEAL_SNAPSHOT_SOFT", None\)',
        "AC2",
        build,
    )
    # Chaos binary must already cover the 4 hard-fail counters per
    # issue body AC2 (pani[residual_panic], LayoutStamp, MutationHold
    # after boundary exit, steal-after-degrade).
    must("steal_snapshot_hard_fail_total", "AC2", chaos)
    must("join_drain_residual_still_running", "AC2", chaos)
    must("mutation_steal_snapshot_mismatch_total", "AC2", chaos)
    # residual_panic check is exercised via AURA_CHAOS_FAULT + arm/release
    # of gc_defer_pending_panic_for; covered by ac2_inject_residual_panic.
    must("residual_panic", "AC2", chaos)
    # LayoutStamp mismatch counter (fiber.h:418 layout_stamp_resume_mismatch_total).
    must("layout_stamp_resume_mismatch", "AC2", chaos)
    # hard-fail CHECK delta == 0 for silent corruption (AC2 fail-on-mismatch).
    must_match(
        r"CHECK\([^)]*delta\s*==\s*0[^)]*silent corruption",
        "AC2",
        chaos,
    )

    # ============================================================
    # AC3: production envelope documented (workers=8, fibers=256,
    #      duration=300s, seed=1, mb_starve_max=0)
    # ============================================================
    must("workers  : 8", "AC3", build)
    must("fibers   : 256", "AC3", build)
    must("duration : 300s", "AC3", build)
    must("seed     : AURA_CHAOS_SEED=1", "AC3", build)
    must("mb_starve_max=0", "AC3", build)
    # Env-setdefault contracts (overrideable for local iteration but
    # default to the production envelope).
    must('env.setdefault("AURA_CHAOS_SEED", "1")', "AC3", build)
    must('env.setdefault("AURA_CHAOS_WORKERS", "8")', "AC3", build)
    must('env.setdefault("AURA_CHAOS_FIBERS", "256")', "AC3", build)
    must('env.setdefault("AURA_CHAOS_DURATION_S", "300")', "AC3", build)
    must('env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0")', "AC3", build)
    # Generous wall — 300s SOAK + watchdog + overhead.
    must("timeout_s = max(900,", "AC3", build)

    # ============================================================
    # AC4: required for any tag / release candidate — wired in
    #      .github/workflows/release.yml as a required step BEFORE
    #      the release-asset upload step.
    # ============================================================
    must("chaos-soak-hard-gate-2722", "AC4", release)
    # Tag push trigger.
    must_match(
        r"tags:\s*\n\s*-\s*\"v\*\"",
        "AC4",
        release,
    )
    # The chaos gate step must precede the release-asset upload step
    # (softprops/action-gh-release@v3) — if the gate fails, no
    # release assets upload.
    upload_step = release.find("softprops/action-gh-release")
    chaos_step_pos = release.find("chaos-soak-hard-gate-2722")
    if chaos_step_pos == -1 or upload_step == -1:
        fails.append(
            "AC4: chaos-soak-hard-gate-2722 step + softprops/action-gh-release step must both be present in release.yml"
        )
    elif chaos_step_pos >= upload_step:
        fails.append(
            "AC4: chaos-soak-hard-gate-2722 step must PRECEDE softprops/action-gh-release "
            "(fail-closed: gate fail → no release assets)"
        )

    # ============================================================
    # AC5: Soft mode explicitly non-gating under cmd_chaos_soak_hard_gate_2722
    # ============================================================
    must("Soft (metric-only) mode remains available", "AC5", build)
    must("EXPLICITLY non-gating", "AC5", build)
    # AURA_STEAL_SNAPSHOT_SOFT must be available for local iteration
    # (preserved by the chaos harness under AURA_CHAOS_FULL=1 + not SOAK).
    must("AURA_STEAL_SNAPSHOT_SOFT", "AC5", chaos)
    # The chaos harness preserves Soft path (FORBIDDEN under production /
    # SOAK / PR gate, available for local iteration).
    must("Soft steal forbidden under production", "AC5", chaos)

    # ============================================================
    # Self-coverage + build.py wire-up
    # ============================================================
    must("check_chaos_soak_hard_gate_2722", "self", build)
    must("#2722", "self", chaos)

    if fails:
        print("chaos SOAK hard gate (#2722) coverage contract rows failed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("chaos SOAK hard gate (#2722) coverage clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

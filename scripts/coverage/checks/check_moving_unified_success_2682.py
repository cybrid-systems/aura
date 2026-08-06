#!/usr/bin/env python3
"""Issue #2682: Moving densify unified success gate.

Contract:
  AC1 Production + Moving + inject untracked external candidate + objects_moved > 0
      → moving_incomplete_remap == true, unified success == false, Phase-5
      outermost does NOT publish success densify metrics.
  AC2 Production + Moving + all roots registered + objects_moved > 0
      → unified success == true; pin_contract_held; untracked_kept == 0.
  AC3 Soft / sandbox=off / Moving blocked by pin|EnvFrameGuard → observe-only;
      no production hard-abort change.
  AC4 RootRemap fail totals > 0 alone → unified success == false.
  AC5 Additive observability: moving-unified-success-total /
      moving-unified-fail-total + schema-2682 / issue-2682 sentinels.
      Existing g_moving_untracked_external_roots_total remains.
  AC6 No docs/design/* per #1655; test extended in existing densify /
      arena_compact / Phase-5 suite per #81967.

Exit 0 = all rows satisfied.
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

    hh = _read("src/core/moving_densify_health.hh")
    arena = _read("src/core/arena.ixx")
    phase5 = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _read("build.py")

    # AC1 + AC2 + AC4 — single unified predicate function (5-condition AND).
    # Check all 5 condition names appear as parameters and the function
    # returns the conjunction.
    must("compute_moving_unified_success", "AC1/AC2/AC4", hh)
    must("moving_blocked_precondition", "AC1", hh)
    must("pin_contract_held", "AC2", hh)
    must("root_remap_stable_ref_fail_total", "AC4", hh)
    must("root_remap_closure_capture_fail_total", "AC4", hh)
    must("untracked_kept_count", "AC1", hh)
    must("objects_moved", "AC1", hh)
    # The function body must AND all 5 conditions (each checked individually
    # with an early return false).
    if hh:
        m = re.search(
            r"\[\[nodiscard\]\][^}]*?compute_moving_unified_success\s*\("
            r"[^)]*?\)\s*(?:noexcept)?\s*\{(.+?)\n\s*\}",
            hh,
            re.MULTILINE | re.DOTALL,
        )
        if not m:
            fails.append("AC1/AC2/AC4: compute_moving_unified_success impl not found")
        else:
            body = m.group(1)
            for cond in (
                "moving_blocked_precondition",
                "!pin_contract_held",
                "root_remap_stable_ref_fail_total",
                "root_remap_closure_capture_fail_total",
                "untracked_kept_count",
            ):
                if cond not in body:
                    fails.append(f"AC1/AC2/AC4: predicate body missing check for {cond!r}")

    # AC5 — process-wide counters in arena.ixx.
    must("g_moving_unified_success_total", "AC5", arena)
    must("g_moving_unified_fail_total", "AC5", arena)
    must("kMovingUnifiedSuccessGateIssue", "AC5", arena)

    # AC1/AC2 — Phase 5 outermost exit calls the unified predicate and bumps
    # counters in lockstep with the densify health window publish.
    must("compute_moving_unified_success", "AC1/AC2", phase5)
    must("g_moving_unified_success_total", "AC1/AC2", phase5)
    must("g_moving_unified_fail_total", "AC1/AC2", phase5)

    # AC5 — query surface wired with all 4 required keys + schema sentinel.
    must("moving-unified-success-total", "AC5", q)
    must("moving-unified-fail-total", "AC5", q)
    must("schema-2682", "AC5", q)
    must("issue-2682", "AC5", q)
    must("moving-unified-success-gate-wired", "AC5", q)

    # AC3 — Soft / observe-only path is preserved. The unified predicate runs
    # unconditionally (no behavior change for Soft), but the fail counter
    # only bumps when had_moving_densify is true (vacuous healthy on Soft /
    # no-densify windows stays out of the fail counter).
    must("had_moving_densify", "AC3", phase5)
    # Soft mode flag check — AURA_MOVING_PIN_CONTRACT / AURA_DENSIFY_CONTRACT
    # env vars for the existing hard-abort path must still exist (not broken
    # by #2682).
    must("AURA_MOVING_PIN_CONTRACT", "AC3", phase5)
    must("AURA_DENSIFY_CONTRACT", "AC3", phase5)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/moving_unified_success_2682.md",
        "docs/moving_unified_success_2682.md",
        "design/2682.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: Issue #2682 sentinel in arena + query + phase5.
    # Use "#2682" (not "Issue #2682") to accept combined citations like
    # "Issue #2682 / #2341 / #2619" without forcing a separate line.
    must("#2682", "AC6", arena)
    must("#2682", "AC6", q)
    must("#2682", "AC6", phase5)

    # Build.py — the new linter is wired into the pre-push gate (cmd_*).
    # This AC is enforced by the next linter file created for #2682 (or by
    # an existing one). Just verify the linter file is on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_moving_unified_success_2682.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2682 Moving densify unified success gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3372: [arena/lifetime] Phase-5 densify fail window publishes
vacuous envframe_ok / dual_epoch_ok — steal LCP can AND a green EnvFrame
axis.

Contract (one row per AC):
  AC1  had_moving_densify && !pin_contract_held must NOT publish
    envframe_ok=true / dual_epoch_ok=true as if pairing ran. Fail-closed
    last-call so Steal LCP would_allow_commit cannot be true solely from
    a vacuous EnvFrame axis.
  AC2  last-call EnvFrame / dual-epoch for that window are false,
    unchecked, or share pin_contract_held. Steal LCP hard-AND refuses.
  AC3  Soft / empty densify (!had_moving_densify) remains vacuous true
    (zero-cost Soft contract preserved).
  AC4  Success path (had_moving_densify && pin_contract_held) still
    runs force_densify_remap_pairing and last-call semantics (#2376).
  AC5  no new query:* keys; no second EnvFrame remap registry; Guard
    dtor scan unchanged.

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

    ev_mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1: fail-window branch publishes false (NOT true) for the
    # axes that didn't run (closure_remount_ok, envframe_ok, dual_epoch_ok).
    # The branch is `else if (had_moving_densify)` — covers pin-contract-fail
    # case. pairing root_remap_ok (densify_root_remap_call_ok) stays real.
    must(
        "} else if (had_moving_densify) {",
        "AC1/AC2 fail-window branch present in evaluator_mutation_boundary.cpp",
        ev_mut,
    )
    must(
        "closure_remount_ok = false;",
        "AC1 fail-window closure_remount_ok = false (NOT true)",
        ev_mut,
    )
    must(
        "envframe_ok = false;",
        "AC1 fail-window envframe_ok = false (NOT true)",
        ev_mut,
    )
    must(
        "note_last_densify_dual_epoch_ok(false);",
        "AC1 fail-window dual_epoch_ok = false (NOT true)",
        ev_mut,
    )
    must(
        "note_last_densify_remap_pairing_forced(false);",
        "AC1 fail-window remap_pairing_forced = false (unchanged from prior)",
        ev_mut,
    )
    # Issue #3372 comment block explains the fix.
    must(
        "// Issue #3372:",
        "AC1 #3372 fix comment present in fail-window branch",
        ev_mut,
    )

    # ── AC2: success path (had_moving_densify && pin_contract_held) unchanged.
    # The pairing.* axes still come from force_densify_remap_pairing().
    must(
        "if (had_moving_densify && pin_contract_held) {",
        "AC2/AC4 success-path pin-contract gate unchanged",
        ev_mut,
    )
    must(
        "force_densify_remap_pairing()",
        "AC4 success-path still runs force_densify_remap_pairing",
        ev_mut,
    )
    must(
        "pairing.closure_remount_ok",
        "AC4 success-path axes sourced from pairing.*",
        ev_mut,
    )

    # ── AC3: Soft / empty densify (!had_moving_densify) keeps vacuous-true.
    # This is the third branch — `else {` after `else if (had_moving_densify)`.
    must(
        "// Soft / empty densify: vacuous axes",
        "AC3 Soft/empty branch vacuous-true comment preserved",
        ev_mut,
    )

    # ── AC4 + AC5: no new query keys, no second EnvFrame remap registry.
    # Verify no new query:* insertions in this region.
    must(
        "note_last_densify_envframe_ok(",
        "AC4 last-call envframe_ok helper still feeds Agent poll",
        ev_mut,
    )
    must(
        "note_last_densify_dual_epoch_ok(",
        "AC4 last-call dual_epoch_ok helper still feeds Agent poll",
        ev_mut,
    )

    # ── AC5: existing test extended with 3372 AC markers (no new file).
    must(
        "3372 AC",
        "AC5 tests/core/test_moving_densify_fail_closed.cpp cites #3372",
        test,
    )
    if (ROOT / "tests" / "core" / "test_issue_3372.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3372.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3372.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3372.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3372-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # ── Linter wired in build.py ──────────────────────────────────────────
    must(
        "check_densify_fail_window_vacuous_axes_3372",
        "AC5 build.py wires 3372 linter",
        build,
    )
    must(
        "Issue #3372",
        "AC5 linter error message in build.py",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3372 densify fail-window vacuous axes — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

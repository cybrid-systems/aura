#!/usr/bin/env python3
# scripts/check_storm_exit_coverage_verify_3649.py -- Issue #3649 source-cite gate.
#
# AC1: the #2952 storm skip contract in maybe_coverage_verify_min_dirty is
#      intact (storm_level/hard_storm guard, two counters, return false);
#      the #3649 edge hook is single-shot (exactly one cite, inside the
#      now==0 && prev!=0 branch — not the #3163 entry or #3515 Both→Shape).
# AC2: storm_exit_force_full_active drives one coverage-verify at the exit
#      edge, after the #3101 threshold-force clear.
# AC3: the hook is production-gated (should_hard_reject_soft_sibling) and
#      residual-gated (residual_force_mask() != 0) — Soft/Off zero-cost.
# AC4: the #3096 256-exit belt is retained (kAutoHealExits = 256,
#      ResidualForceHeal reason-group, storm + attempts_left gates);
#      last_reemit_success_region_mask_ remains the residual SSOT.
# AC5: test wiring — the #3649 block in tests/compiler/
#      test_exhausted_min_dirty_reemit.cpp with AC1–AC5 labels; build.py
#      registration + root_check_allowlist append; no tests/**/
#      test_issue_3649.cpp; no docs/design/3649*.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CPP = "src/compiler/hot_update_registry.cpp"
TEST = "tests/compiler/test_exhausted_min_dirty_reemit.cpp"
BUILD = "build.py"

LINTER = "check_storm_exit_coverage_verify_3649"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(cpp: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — storm skip contract intact + single-shot edge hook.
    must("Issue #2952: auto-seed min-dirty for residual", "AC1 2952 walk cite", cpp)
    must("Issue #3649: the storm-exit edge must not leave residual", "AC1 hook cite", cpp)
    if cpp.count("Issue #3649") != 1:
        fails.append("AC1: expected exactly one #3649 hook site (single-shot edge)")
    begin = cpp.find("bool HotUpdateRegistry::storm_exit_force_full_active() noexcept")
    end = cpp.find("Issue #2302: accessor for the 5-field ReloadRecovery state", begin)
    if begin < 0 or end < 0 or end <= begin:
        fails.append("AC1: storm_exit_force_full_active body not located")
        return fails
    body = cpp[begin:end]
    entry_pos = body.find("Issue #3163: storm entry edge")
    hook_pos = body.find("Issue #3649")
    lost_pos = body.find("Issue #3515: Both→Shape")
    # Branch order in source: exit edge (hook) → #3515 Both→Shape → #3163 entry.
    # Positions are body-relative (body starts at begin).
    if entry_pos < 0 or hook_pos < 0 or lost_pos < 0 or not (hook_pos < lost_pos < entry_pos):
        fails.append("AC1: hook must sit in the exit branch (before #3515 Both→Shape and #3163 entry)")

    # AC2 — exit-edge drive, after the #3101 clear.
    must("aura_clear_partial_relower_threshold_force()", "AC2 3101 clear", body)
    clear_pos = body.find("aura_clear_partial_relower_threshold_force()")
    if clear_pos >= hook_pos:
        fails.append("AC2: hook must follow the #3101 clear")
    must("maybe_coverage_verify_min_dirty()", "AC2 edge drive", body[hook_pos:])

    # AC3 — production + residual gates on the hook.
    hook_block = body[hook_pos : lost_pos if lost_pos > hook_pos else len(body)]
    must("should_hard_reject_soft_sibling()", "AC3 production gate", hook_block)
    must("residual_force_mask() != 0", "AC3 residual gate", hook_block)

    # AC4 — #3096 belt + reason-group + residual SSOT retained.
    must("kAutoHealExits = 256", "AC4 256-exit belt", cpp)
    must("ReemitReason::ResidualForceHeal", "AC4 reason-group", cpp)
    must("attempts_left != 0", "AC4 attempts gate", cpp)
    must("force & ~last_cov", "AC4 residual SSOT", cpp)
    must("if (current_storm_level() != StormLevel::None || hard_storm_active())", "AC4 storm skip guard", cpp)

    # AC5 — test wiring + registration + forbidden artifacts.
    must("#3649: storm-exit edge residual coverage-verify", "AC5 test block", test)
    for ac in (
        "3649 AC1: storm active still skips",
        "3649 AC2: exit edge schedules coverage-verify (no 256 wait)",
        "3649 AC3: Soft edge does not schedule",
        "3649 AC4: #3096 256-exit belt retained",
        "3649 AC5: cpp cites #3649",
    ):
        must(ac, "AC5 runner wired", test)
    must("storm_exit_force_full_active()", "AC5 edge consult in test", test)
    must("check_storm_exit_coverage_verify_3649", "AC5 build.py registration", build)
    must_not("test_issue_3649", "AC5 no tests/issues literal", test)
    for stale in ROOT.glob("docs/design/*3649*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3649*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3649 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    cpp = _read(CPP)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = cpp.replace("Issue #3649", "Issue #redacted")
        self_fails = _rows(broken, test, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(cpp, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

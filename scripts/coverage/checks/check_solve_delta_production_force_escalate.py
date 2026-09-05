#!/usr/bin/env python3
"""Issue #3557 linter: enforce the production force-escalate solve_delta
cap-hit-to-full wire is present in solve_delta_impl + the supporting
file-scope atomic + accessors are wired.

Without this guard, cap-hit residual in pending_full_solve_roots_ is
consumed only by the NEXT delta's solve_delta pass \u2014 under production,
half-solved type state propagates between calls (residual #G2 \u2014 typed
mutation \xd7 type-system review).

Usage:
    python3 scripts/coverage/checks/check_solve_delta_production_force_escalate.py --strict

Located under scripts/coverage/checks/ per tests/COVERAGE.md (the
scripts/check_*.py root is FROZEN).

Required (all 5):
    - solve_delta_impl tail has a new check BEFORE the existing
      hard && last_reverify_truncated_ check (returns first when
      strict production + truncated + non-empty residual).
    - The new check uses production_defaults_active() (NOT hard) \u2014
      strict production gate (Full strategy alone does NOT trigger).
    - The new check calls
      escalate_if_production(SolveResult::TIMEOUT, unresolved_out) and
      returns the result (same path as natural TIMEOUT).
    - The new check bumps the new counter via
      bump_solve_delta_full_solve_force_escalate_total() (file-scope
      atomic).
    - The new counter g_solve_delta_full_solve_force_escalate_total
      + accessor + reset_for_test + bump_for_test are present in
      type_checker_impl.cpp (file-scope, anonymous namespace).

Forbidden regression (must NOT regress):
    - existing hard && last_reverify_truncated_ check (broader #3511
      family) preserved.
    - existing cap-hit frontier push at :1205-1209 still enqueues
      residual in pending_full_solve_roots_ (the trigger for the new
      check).
    - existing solver_budget_full_escalate_total bump in
      escalate_if_production preserved (sibling counter).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent.parent
TCI = ROOT / "src" / "compiler" / "type_checker_impl.cpp"

# Required: new file-scope atomic + accessors.
RE_FILE_ATOMIC = re.compile(r"std::atomic<std::uint64_t>\s+g_solve_delta_full_solve_force_escalate_total\{0\}")
RE_V_READ = re.compile(r"g_solve_delta_full_solve_force_escalate_total_v_read\(\)\s*noexcept")
RE_BUMP = re.compile(r"bump_solve_delta_full_solve_force_escalate_total\(\)\s*noexcept")
RE_BUMP_TEST = re.compile(r"bump_solve_delta_full_solve_force_escalate_total_for_test\(\)\s*noexcept")
RE_RESET_TEST = re.compile(r"reset_solve_delta_full_solve_force_escalate_total_for_test\(\)\s*noexcept")
# Required: new check wired in solve_delta_impl tail.
# The new check has 3 conditions: production_defaults_active() &&
# last_reverify_truncated_ && !pending_full_solve_roots_.empty()
RE_NEW_CHECK_GUARD = re.compile(
    r"production_defaults_active\(\)\s*&&\s*"
    r"last_reverify_truncated_\s*&&\s*"
    r"!pending_full_solve_roots_\.empty\(\)"
)
# Required: new check calls escalate_if_production(TIMEOUT, unresolved_out)
# and bumps the new counter.
RE_NEW_CHECK_ESCALATE = re.compile(r"escalate_if_production\(SolveResult::TIMEOUT,\s*unresolved_out\)")
# Required: existing #3511 family preserved.
RE_HARD_CHECK = re.compile(r"hard\s*&&\s*last_reverify_truncated_")
RE_CAP_HIT_PUSH = re.compile(r"pending_full_solve_roots_\.insert\(frontier\[j\]\)")
RE_FULL_ESCALATE_BUMP = re.compile(r"c\.solver_budget_full_escalate_total\.fetch_add\(1")
# Required: new check is BEFORE the existing hard check in source order.
# (This is verified by positional check, not regex.)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--strict", action="store_true", help="Fail on missing wire patterns (deploy gate red)")
    args = p.parse_args()
    strict = bool(args.strict)

    v = 0
    if not TCI.exists():
        fail(f"missing {TCI}")
        return 1 if strict else 0

    text = TCI.read_text(encoding="utf-8", errors="replace")

    checks = [
        # AC1: file-scope atomic + accessors wired.
        (
            "AC1",
            text,
            RE_FILE_ATOMIC,
            "type_checker_impl.cpp: file-scope atomic g_solve_delta_full_solve_force_escalate_total{0} present",
        ),
        (
            "AC1",
            text,
            RE_V_READ,
            "type_checker_impl.cpp: g_solve_delta_full_solve_force_escalate_total_v_read() accessor",
        ),
        (
            "AC1",
            text,
            RE_BUMP,
            "type_checker_impl.cpp: bump_solve_delta_full_solve_force_escalate_total() helper (called in reject path)",
        ),
        (
            "AC1",
            text,
            RE_BUMP_TEST,
            "type_checker_impl.cpp: bump_solve_delta_full_solve_force_escalate_total_for_test() test seam",
        ),
        (
            "AC1",
            text,
            RE_RESET_TEST,
            "type_checker_impl.cpp: reset_solve_delta_full_solve_force_escalate_total_for_test() test seam",
        ),
        # AC1: new 3-condition guard wired in solve_delta_impl tail.
        (
            "AC1",
            text,
            RE_NEW_CHECK_GUARD,
            "type_checker_impl.cpp: new 3-condition guard "
            "production_defaults_active() && last_reverify_truncated_ && !pending_full_solve_roots_.empty()",
        ),
        # AC1: new check calls escalate_if_production(TIMEOUT, unresolved_out).
        (
            "AC1",
            text,
            RE_NEW_CHECK_ESCALATE,
            "type_checker_impl.cpp: new check calls escalate_if_production(TIMEOUT, unresolved_out)",
        ),
        # AC2: Soft / Off path — new check uses strict production gate.
        # (verified via source-cite: the new check is BEFORE the broader
        # hard check, so Soft path falls through to the existing #3511 path
        # without bumping the new counter.)
        (
            "AC2",
            text,
            RE_HARD_CHECK,
            "type_checker_impl.cpp: existing #3511 hard && last_reverify_truncated_ check preserved (Soft / Off path)",
        ),
        # AC3: existing cap-hit frontier push + flag + sibling counter preserved.
        (
            "AC3",
            text,
            RE_CAP_HIT_PUSH,
            "type_checker_impl.cpp: existing cap-hit frontier push at :1205-1209 preserved (trigger for new check)",
        ),
        (
            "AC3",
            text,
            RE_FULL_ESCALATE_BUMP,
            "type_checker_impl.cpp: existing solver_budget_full_escalate_total bump in "
            "escalate_if_production preserved (sibling counter)",
        ),
    ]
    for label, t, regex, why in checks:
        if not regex.search(t):
            fail(f"{label}: {regex.pattern!r}: missing ({why})")
            v += 1

    # Positional check: new check BEFORE existing hard check.
    pos_bump_call = text.find("bump_solve_delta_full_solve_force_escalate_total()")
    pos_hard_check = text.find("hard && last_reverify_truncated_")
    if pos_bump_call < 0 or pos_hard_check < 0:
        fail("AC1: cannot find both new check and hard check for positional verification")
        v += 1
    elif pos_bump_call >= pos_hard_check:
        fail(
            f"AC1: new check at byte {pos_bump_call} must be BEFORE hard check at byte {pos_hard_check} "
            f"(returns first when prod+residual)"
        )
        v += 1

    if v > 0 and strict:
        print(
            f"\ncheck_solve_delta_production_force_escalate: {v} violation(s) \u2014 refusing to ship",
            file=sys.stderr,
        )
        return 1
    if v > 0:
        print(
            f"check_solve_delta_production_force_escalate: {v} warning(s) (run with --strict to enforce)",
            file=sys.stderr,
        )
        return 0
    print(
        "check_solve_delta_production_force_escalate: OK "
        "(3557 AC: 3-condition guard + escalate + #3511 family + positional order)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

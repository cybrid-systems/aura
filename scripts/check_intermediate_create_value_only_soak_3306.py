#!/usr/bin/env python3
# scripts/check_intermediate_create_value_only_soak_3306.py — Issue #3306 source-cite gate.
#
# Verifies that the densify entry fail-close block in src/core/arena.ixx
# also fail-closes when intermediate_create_value_only_total > 0 under
# required (defense-in-depth for the soak invariant). Closes the
# dual-track residual where older call sites still hit
# note_intermediate_create_auto_wire_ under required densify-tracked
# allocates — the existing has_unpinned_intermediate_creates_() scan
# should already catch the push_back, but this OR clause belt-and-suspenders
# the soak invariant (value_only_total == 0 under production required,
# per AC2 of #3306).
#
# Contract rows (AC1–AC4 from the test file):
#
#   AC1: densify entry fail-close OR-condition includes
#        intermediate_create_value_only_total_v_read() > 0
#   AC2: existing fail-close fields preserved (pin_contract_held=false,
#        moving_incomplete_remap=true, moving_blocked_precondition=true,
#        soft_gated=true, sticky densify-off via
#        g_moving_incomplete_remap_sticky_densify_off)
#   AC3: comment block documents #3306 defense-in-depth close
#   AC4: no docs/design/3306-* (per #1655); no test_issue_3306.cpp
#        (per #81934 — extend existing test_general_object_pin_coverage_gate)
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_intermediate_create_value_only_soak_3306.py --self-test

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = ("src/core/arena.ixx",)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_fail_close_or_condition(arena: str) -> list[str]:
    """AC1: densify entry fail-close OR-condition includes value_only_total > 0."""
    failures: list[str] = []
    needle = (
        "aura::core::lifetime::general_object_pin_required_active() &&\n"
        "                (has_unpinned_intermediate_creates_() ||\n"
        "                 intermediate_create_value_only_total_v_read() > 0)"
    )
    if needle not in arena:
        failures.append(
            "AC1: densify entry fail-close does not include "
            "intermediate_create_value_only_total_v_read() > 0 OR-condition under required"
        )
    return failures


def _check_fail_close_fields_preserved(arena: str) -> list[str]:
    """AC2: existing fail-close fields preserved."""
    failures: list[str] = []
    # Find the fail-close block anchor (the new OR-condition we just added).
    fail_close_pos = arena.find("intermediate_create_value_only_total_v_read() > 0)")
    if fail_close_pos < 0:
        # Fail-close anchor not found — AC1 already failed, skip AC2.
        return failures
    scope = arena[fail_close_pos : fail_close_pos + 1500]
    required_fields = (
        ("result.pin_contract_held = false", "AC2: pin_contract_held=false"),
        ("result.moving_incomplete_remap = true", "AC2: moving_incomplete_remap=true"),
        ("result.moving_blocked_precondition = true", "AC2: moving_blocked_precondition=true"),
        ("result.soft_gated = true", "AC2: soft_gated=true"),
        (
            "g_moving_incomplete_remap_sticky_densify_off.exchange(",
            "AC2: sticky densify-off via g_moving_incomplete_remap_sticky_densify_off",
        ),
    )
    for needle, msg in required_fields:
        if needle not in scope:
            failures.append(msg)
    return failures


def _check_comment_documents(arena: str) -> list[str]:
    """AC3: comment block documents #3306 defense-in-depth close."""
    failures: list[str] = []
    if "Issue #3306: defense-in-depth" not in arena:
        failures.append("AC3: comment does not document the #3306 defense-in-depth close")
    return failures


def run_strict() -> list[str]:
    arena = _read("src/core/arena.ixx")
    failures: list[str] = []
    failures.extend(_check_fail_close_or_condition(arena))
    failures.extend(_check_fail_close_fields_preserved(arena))
    failures.extend(_check_comment_documents(arena))
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3306 source-cite checks pass")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    ap.add_argument(
        "--self-test", action="store_true", help="Run the linter against the current repo; expect zero failures."
    )
    ap.add_argument(
        "--strict", action="store_true", default=True, help="Default mode: emit failures and exit non-zero on any."
    )
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures = run_strict()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #3306 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())

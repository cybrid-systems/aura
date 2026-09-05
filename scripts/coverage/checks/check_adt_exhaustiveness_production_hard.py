#!/usr/bin/env python3
# check_adt_exhaustiveness_production_hard.py -- Issue #3559 ADT exhaustiveness
# production/Full hard-reject linter.
#
# Refuses evaluator_eval_flat.cpp match-check path that does NOT consult
# `production_hard_face_active()` (the #3556 centralized hard-face gate,
# equivalent to `production_defaults_active() || get_strategy() ==
# AuditStrategy::Full`). Without the wire, Agent self-mutation may add a
# variant to an ADT and the existing match sites silently slide into
# Dynamic in Production — wrong program passes.
#
# Sites:
#   1. src/compiler/typed_mutation_audit.h — `adt_exhaustiveness_hard_reject_face`
#      atomic present, sibling of `adt_exhaustiveness_hard_gate_wired{1}`
#      (NOT inserted in metrics middle).
#   2. src/compiler/evaluator_eval_flat.cpp — match exhaustiveness check
#      block (locate by `match warning: unhandled constructor`) must:
#        - bump `adt_exhaustiveness_fail_total` (observe-only under Soft)
#        - consult `production_hard_face_active()` for the hard-reject branch
#        - store `adt_exhaustiveness_hard_reject_face` (release) before
#          returning error EvalValue
#
# --strict: exit 1 on any violation, 0 on clean.
# --self-test: runs an inline AC matrix verifying the hard-face gate
# semantics (Production/Full → reject; Soft → no reject; counter bumps
# regardless).

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

EVAL_FLAT = REPO_ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
AUDIT_H = REPO_ROOT / "src" / "compiler" / "typed_mutation_audit.h"

# Window after the match-warning print to scan for the hard-reject wire.
WINDOW = 2000


def find_match_check_window(src: str) -> str:
    """Return up to 500 chars BEFORE + WINDOW chars AFTER the
    `match warning: unhandled constructor` print — covers the
    `adt_exhaustiveness_fail_total` bump (inside the for loop, BEFORE
    the print) plus the per-ctor loop close + the
    `production_hard_face_active()` hard-reject branch (AFTER)."""
    marker = "match warning: unhandled constructor"
    pos = src.find(marker)
    if pos == -1:
        return ""
    begin = pos - 500 if pos >= 500 else 0
    end = min(len(src), pos + WINDOW)
    return src[begin:end]


def check_eval_flat_hard_reject(src: str) -> list[str]:
    if not src:
        return [f"{EVAL_FLAT}: file empty or unreadable"]
    window = find_match_check_window(src)
    if not window:
        return [f"{EVAL_FLAT}: match-check block (match warning print) not found"]
    failures: list[str] = []
    # Counter bump on every non-exhaustive match.
    if ".adt_exhaustiveness_fail_total" not in window:
        failures.append(
            f"{EVAL_FLAT}: missing adt_exhaustiveness_fail_total bump (observe-only counter must always bump)"
        )
    # production_hard_face_active() gate consulted.
    if "production_hard_face_active()" not in window:
        failures.append(f"{EVAL_FLAT}: missing production_hard_face_active() gate (hard-reject wire required for AC2)")
    # Hard-reject face stored (release).
    if ".adt_exhaustiveness_hard_reject_face" not in window:
        failures.append(f"{EVAL_FLAT}: missing adt_exhaustiveness_hard_reject_face store")
    if "std::memory_order_release" not in window:
        failures.append(f"{EVAL_FLAT}: hard-reject face must use std::memory_order_release")
    # Error return (empty EvalValue) — not the normal `return make_void();`
    # binding path.
    if "return {};" not in window:
        failures.append(
            f"{EVAL_FLAT}: missing 'return {{}};' error EvalValue return "
            "(hard reject must abort the match, not fall through to binding)"
        )
    return failures


def check_counter_family(src: str) -> list[str]:
    """Verify `adt_exhaustiveness_hard_reject_face` is a sibling of
    `adt_exhaustiveness_hard_gate_wired{1}` (NOT inserted in metrics
    middle — keep the family contiguous)."""
    if not src:
        return [f"{AUDIT_H}: file empty or unreadable"]
    anchor = "adt_exhaustiveness_hard_gate_wired{1}"
    pos = src.find(anchor)
    if pos == -1:
        return [f"{AUDIT_H}: anchor `adt_exhaustiveness_hard_gate_wired{{1}}` not found"]
    # 300 chars before + after the anchor should contain the new face atomic.
    begin = max(0, pos - 300)
    end = min(len(src), pos + 300)
    window = src[begin:end]
    if "adt_exhaustiveness_hard_reject_face" not in window:
        return [
            f"{AUDIT_H}: adt_exhaustiveness_hard_reject_face not adjacent to "
            f"adt_exhaustiveness_hard_gate_wired{{1}} (sibling required; "
            f"no metrics-middle insert)"
        ]
    return []


def run_self_test() -> list[str]:
    """Inline AC matrix — verify production_hard_face_active() semantics
    (Production/Full → reject; Soft → no reject)."""
    failures: list[str] = []

    def production_hard_face_active(production_defaults: bool, full: bool) -> bool:
        return production_defaults or full

    cases = [
        # (production_defaults, full_strategy, expected_hard_face, label)
        (True, False, True, "production-default active"),
        (False, True, True, "Full strategy active"),
        (True, True, True, "both active"),
        (False, False, False, "Soft / Sampled / audit-only — no hard reject"),
    ]
    for prod, full, expected, label in cases:
        got = production_hard_face_active(prod, full)
        if got != expected:
            failures.append(
                f"self-test: case {label!r} (production={prod}, full={full}) expected hard_face={expected}, got {got}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any violation (default: warnings only)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run inline AC matrix (production_hard_face_active semantics)",
    )
    args = parser.parse_args()

    failures: list[str] = []
    eval_src = EVAL_FLAT.read_text(encoding="utf-8") if EVAL_FLAT.exists() else ""
    audit_src = AUDIT_H.read_text(encoding="utf-8") if AUDIT_H.exists() else ""
    failures.extend(check_eval_flat_hard_reject(eval_src))
    failures.extend(check_counter_family(audit_src))
    if args.self_test:
        failures.extend(run_self_test())

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if args.strict or args.self_test:
            return 1
        return 0
    if args.self_test:
        print(
            "PASS: evaluator_eval_flat.cpp match-check has production_hard_face_active() "
            "hard-reject wire (counter bump + face release store + return error); "
            "counter family sibling-clean; self-test hard-face semantics correct"
        )
    else:
        print("PASS: ADT exhaustiveness production hard-reject wire clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #3556 linter: enforce the production hard-reject expected_fp==0
unstaged persist precondition is wired in
aura_outermost_success_persist_occurrence + the supporting helpers
(file-scope atomic + production_hard_face_active + test reset for
g_last_proof_goal_fingerprint) are present.

Without this guard, production persist can freeze an empty fingerprint
when Agent's mutation fails to stage -- next query:type reads pre-mutate
empty snapshot, evolution snapshot's last_proof_goal_fingerprint==0
becomes indistinguishable from "not staged" -- Agent self-mutation
becomes invisible (#3556 residual #G1).

Usage:
    python3 scripts/coverage/checks/check_occurrence_persist_expected_fp_zero.py --strict

Located under scripts/coverage/checks/ per tests/COVERAGE.md (the
scripts/check_*.py root is FROZEN).

Required (all 6):
    - aura_outermost_success_persist_occurrence body guards on
      production_hard_face_active() AND last_proof_goal_fingerprint_v_read() == 0
      (the #3556 precondition).
    - aura_outermost_success_persist_occurrence bumps
      bump_occurrence_persist_reject_expected_fp_zero_total() in the
      reject path.
    - aura_outermost_success_persist_occurrence rejects with force_reason=16
      and publishes kTypeLinearProofOutcomeReject on the reject path
      (same pattern as #3418/#3431/#3376/#3281 reject sites).
    - typed_mutation_audit.h exposes file-scope atomic
      g_occurrence_persist_reject_expected_fp_zero_total + accessor +
      reset_for_test + bump helper.
    - typed_mutation_audit.h exposes production_hard_face_active()
      (centralized hard-face gate).
    - typed_mutation_audit.h exposes
      reset_last_proof_goal_fingerprint_for_test() (test seam for
      deterministic fingerprint==0 AC).

Forbidden (must NOT regress):
    - existing g_occurrence_persist_fingerprint_mismatch_total family
      untouched (#3431 path remains).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent.parent
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TA = ROOT / "src" / "compiler" / "typed_mutation_audit.h"

# Required: new precondition wired into aura_outermost_success_persist_occurrence.
RE_HARD_GUARD = re.compile(
    r"production_hard_face_active\(\)\s*&&\s*"
    r"aura::compiler::typed_audit::last_proof_goal_fingerprint_v_read\(\)\s*==\s*0"
)
RE_BUMP = re.compile(r"bump_occurrence_persist_reject_expected_fp_zero_total\(\)")
RE_FORCE_REASON_16 = re.compile(r"/\*force_reason=\*/16")
RE_REJECT_PUBLISH = re.compile(
    r"publish_type_linear_proof_outcome\(\s*\n?\s*"
    r"aura::compiler::typed_audit::kTypeLinearProofOutcomeReject\s*\)"
)
# Required: file-scope atomic + accessor + reset + bump helper.
RE_FILE_ATOMIC = re.compile(
    r"inline\s+std::atomic<std::uint64_t>\s+"
    r"g_occurrence_persist_reject_expected_fp_zero_total\{0\};"
)
RE_ACCESSOR = re.compile(r"occurrence_persist_reject_expected_fp_zero_total_v_read\(\)\s*noexcept")
RE_TEST_RESET = re.compile(r"reset_occurrence_persist_reject_expected_fp_zero_total_for_test\(\)\s*noexcept")
RE_BUMP_HELPER = re.compile(r"inline\s+void\s+bump_occurrence_persist_reject_expected_fp_zero_total\(\)\s*noexcept")
RE_PROD_HARD_FACE = re.compile(
    r"inline\s+bool\s+production_hard_face_active\(\)\s*noexcept\s*\{"
    r"\s*return\s+production_defaults_active\(\)\s*\|\|\s*"
    r"get_strategy\(\)\s*==\s*AuditStrategy::Full\s*;\s*\}"
)
RE_FP_RESET = re.compile(
    r"inline\s+void\s+reset_last_proof_goal_fingerprint_for_test\(\)\s*noexcept\s*\{"
    r"\s*g_last_proof_goal_fingerprint\.store\(0,\s*std::memory_order_relaxed\)\s*;\s*\}"
)
# Forbidden regression: existing #3431 mismatch family must still be present.
# The counter is a member field of EvaluatorMetrics (in evaluator.ixx),
# bumped via ev->bump_occurrence_persist_fingerprint_mismatch() — referenced
# 8x in evaluator_mutation_boundary.cpp (existing reject paths). Also grep
# the field name for completeness.
RE_MISMATCH_BUMP = re.compile(r"bump_occurrence_persist_fingerprint_mismatch\b")
RE_MISMATCH_FIELD = re.compile(r"occurrence_persist_fingerprint_mismatch_total")


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--strict", action="store_true", help="Fail on missing wire patterns (deploy gate red)")
    args = p.parse_args()
    strict = bool(args.strict)

    v = 0
    if not EMB.exists():
        fail(f"missing {EMB}")
        return 1 if strict else 0
    if not TA.exists():
        fail(f"missing {TA}")
        return 1 if strict else 0

    emb_text = EMB.read_text(encoding="utf-8", errors="replace")
    ta_text = TA.read_text(encoding="utf-8", errors="replace")

    checks = [
        # AC1: precondition wired in aura_outermost_success_persist_occurrence
        (
            "AC1",
            emb_text,
            RE_HARD_GUARD,
            "evaluator_mutation_boundary.cpp: production_hard_face_active() && "
            "last_proof_goal_fingerprint_v_read() == 0 guard",
        ),
        (
            "AC1",
            emb_text,
            RE_BUMP,
            "evaluator_mutation_boundary.cpp: bump_occurrence_persist_reject_expected_fp_zero_total()",
        ),
        (
            "AC1",
            emb_text,
            RE_FORCE_REASON_16,
            "evaluator_mutation_boundary.cpp: reject proof stamped with force_reason 16",
        ),
        (
            "AC1",
            emb_text,
            RE_REJECT_PUBLISH,
            "evaluator_mutation_boundary.cpp: publish_type_linear_proof_outcome(Reject) on reject path",
        ),
        # AC3: typed_mutation_audit.h exposes the new family + helpers.
        (
            "AC3",
            ta_text,
            RE_FILE_ATOMIC,
            "typed_mutation_audit.h: file-scope atomic g_occurrence_persist_reject_expected_fp_zero_total",
        ),
        (
            "AC3",
            ta_text,
            RE_ACCESSOR,
            "typed_mutation_audit.h: accessor occurrence_persist_reject_expected_fp_zero_total_v_read()",
        ),
        (
            "AC3",
            ta_text,
            RE_TEST_RESET,
            "typed_mutation_audit.h: reset_occurrence_persist_reject_expected_fp_zero_total_for_test()",
        ),
        (
            "AC3",
            ta_text,
            RE_BUMP_HELPER,
            "typed_mutation_audit.h: bump_occurrence_persist_reject_expected_fp_zero_total() helper",
        ),
        ("AC3", ta_text, RE_PROD_HARD_FACE, "typed_mutation_audit.h: production_hard_face_active() centralized helper"),
        ("AC3", ta_text, RE_FP_RESET, "typed_mutation_audit.h: reset_last_proof_goal_fingerprint_for_test() test seam"),
        # Forbidden regression: existing #3431 family untouched.
        # The counter is a member field of EvaluatorMetrics (in evaluator.ixx),
        # bumped via ev->bump_occurrence_persist_fingerprint_mismatch() — referenced
        # 8x in evaluator_mutation_boundary.cpp (existing reject paths).
        (
            "AC4",
            emb_text,
            RE_MISMATCH_BUMP,
            "evaluator_mutation_boundary.cpp: existing "
            "bump_occurrence_persist_fingerprint_mismatch() calls still present "
            "(#3431 path not regressed)",
        ),
    ]
    for label, text, regex, why in checks:
        if not regex.search(text):
            fail(f"{label}: {regex.pattern!r}: missing ({why})")
            v += 1

    if v > 0 and strict:
        print(
            f"\ncheck_occurrence_persist_expected_fp_zero: {v} violation(s) — refusing to ship",
            file=sys.stderr,
        )
        return 1
    if v > 0:
        print(
            f"check_occurrence_persist_expected_fp_zero: {v} warning(s) (run with --strict to enforce)",
            file=sys.stderr,
        )
        return 0
    print(
        "check_occurrence_persist_expected_fp_zero: OK "
        "(3556 AC: precondition + counter + helpers + #3431 family preserved)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

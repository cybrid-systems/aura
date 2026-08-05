#!/usr/bin/env python3
"""check_composite_drift_inject_2671.py — Issue #2671 source gate.

Drift-injection soak for OccurrenceGoal refined consistency (refine #2644).
Closes the residual "SOLVED-but-drift" class that the #2644 check
introspected but could not hermetically exercise: seeds two live
OccurrenceGoal rows on the same UF rep with incompatible refined (int vs
string) so check_occurrence_refined_consistency() detects bidirectional
consistent_unify failure. composite_txn_commit routes Soft observe vs
Full/strict reject based on typed_audit::production_defaults_active().

AC1: Soft + injected incompatible refined → observe++, not rejected
AC2: production/Full + same inject → reject++, rejected=true
AC3: compatible refined / single goal → zero extra (covered by #2644 AC4)
AC4: empty occurrence_goals_ → zero counter bump (covered by #2644 AC4)
AC5: #2610 empty-CS / auto_partial matrix unchanged (inject does not fake
     empty CS)
AC6: src-aligned test (extend test_composite_commit_cs_reuse.cpp #2180 /
     #2644 suite per #81967) + coverage gate (this linter + build.py
     cmd_composite_drift_inject_2671_coverage).

Default: non-strict (exit 0, prints coverage summary). Use --strict to
enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
EVALUATOR_IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EVALUATOR_TYPECHECK = ROOT / "src" / "compiler" / "evaluator_typecheck.cpp"
AUDIT_H = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
BUILD = ROOT / "build.py"
TEST = ROOT / "tests" / "compiler" / "test_composite_commit_cs_reuse.cpp"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    def must_present(path: Path, needle: str, label: str) -> None:
        if not path.exists():
            failures.append(f"{label}: {path} not found")
            return
        text = path.read_text(encoding="utf-8", errors="replace")
        if needle not in text:
            failures.append(f"{label}: missing {needle!r} in {path.name}")

    ixx_text = _read("src/compiler/evaluator.ixx")  # noqa: F841 — used via must_present
    tc_text = _read("src/compiler/evaluator_typecheck.cpp")  # noqa: F841 — used via must_present
    audit_text = _read("src/compiler/typed_mutation_audit.h")  # noqa: F841 — used via must_present

    # AC1+AC2: helper declaration + definition + drift-injection semantics.
    must_present(
        EVALUATOR_IXX,
        "inject_commit_occurrence_drift_for_test",
        "AC1: evaluator.ixx declares drift-injection helper",
    )
    must_present(
        EVALUATOR_TYPECHECK,
        "Issue #2671: drift-injection soak",
        "AC2: evaluator_typecheck.cpp cites #2671 helper intent",
    )
    must_present(
        EVALUATOR_TYPECHECK,
        "inject_commit_occurrence_drift_for_test() noexcept",
        "AC2: evaluator_typecheck.cpp defines drift-injection helper",
    )
    must_present(
        EVALUATOR_TYPECHECK,
        "note_occurrence_goal",
        "AC2: helper uses note_occurrence_goal (public CS API)",
    )

    # AC1: Soft path — observe counter advance on drift, no reject.
    # AC2: Full / production path — reject counter advance, rejected=true.
    # Both routed by composite_txn_commit which already wires the
    # check_occurrence_refined_consistency() helper from #2644; verify
    # the production_defaults_active() routing is preserved.
    must_present(
        EVALUATOR_TYPECHECK,
        "check_occurrence_refined_consistency",
        "AC1: composite_txn_commit still calls #2644 check helper",
    )
    must_present(
        EVALUATOR_TYPECHECK,
        "production_defaults_active()",
        "AC2: production/Soft routing preserved (no hard-coded full)",
    )

    # Counters from #2644 preserved (additive, not replaced).
    must_present(
        AUDIT_H,
        "composite_type_scheme_drift_observe_total",
        "AC1: observe counter in TypedMutationAuditCounters",
    )
    must_present(
        AUDIT_H,
        "composite_type_scheme_drift_reject_total",
        "AC2: reject counter in TypedMutationAuditCounters",
    )

    # AC6: test file coverage — #2671 ACs present + run_test_ invokes them.
    test_text = _read("tests/compiler/test_composite_commit_cs_reuse.cpp")
    for ac_fn in (
        "ac2671_soft_drift_observe",
        "ac2671_full_drift_reject",
        "ac2671_2610_empty_cs_unchanged",
        "ac2671_schema_and_source",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test missing {ac_fn} function")
    if "run_test_composite_commit_cs_reuse" in test_text:
        for ac_fn in (
            "ac2671_soft_drift_observe",
            "ac2671_full_drift_reject",
            "ac2671_2610_empty_cs_unchanged",
            "ac2671_schema_and_source",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: run_test does not call {ac_fn}()")

    # AC6: build.py wiring.
    build_text = _read("build.py")
    if "check_composite_drift_inject_2671" not in build_text:
        failures.append("AC6: build.py does not reference check_composite_drift_inject_2671 linter")
    if "cmd_composite_drift_inject_2671_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_composite_drift_inject_2671_coverage function")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print(
        "OK: all #2671 ACs satisfied (drift-injection soak for OccurrenceGoal "
        "refined consistency — hermetic helper + Soft observe / Full reject "
        "routing, additive to #2644/#2610/#2180)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

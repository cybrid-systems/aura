#!/usr/bin/env python3
"""Issue #3190: outermost-success TypeLinearCommitProof drain before stamp.

Sibling #3031 closes the composite_txn_commit drain window. #3190 closes
the outermost-success stamp window (aura_outermost_success_persist_occurrence)
that the next composite commit / outermost stamp could observe as SOLVED
even though pending_full_solve_roots_ / locality residual remained. The
drain runs at the outermost success stamp site under production/Full/Strict;
Soft observe-only; Quiet (no residual) → two size reads, zero extra atomics.
Lockless batch (atomic_batch_active) flows through composite_txn_commit
body, which already has the same drain — #3190 AC4 verifies the coverage.

Contract:
  AC1 Production drain at outermost stamp reject → force_reason 16
  AC2 Soft: observe allow
  AC3 Quiet residual 0: no extra counters
  AC4 lockless batch covered by composite_txn_commit drain
  AC5 existing #2913 / #2994 / #3031 / #3169 surfaces preserved
  AC6 extend test_solve_delta_unresolved_export; no docs/design / invent;
     coverage linter wired into build.py

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    ev_mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    # AC1 — drain helper exists and is reachable from outermost success stamp.
    must("drain_pending_full_solve_before_commit", "AC1", ixx)
    must("drain_pending_full_solve_before_commit", "AC1", impl)
    must("drain_pending_full_solve_before_commit", "AC1", ev_mb)
    must("Issue #3190", "AC1", ev_mb)
    must("force_reason=*/16", "AC1", ev_mb)
    must("force_reason=*/16", "AC1", tc)
    must("publish_type_linear_proof_outcome", "AC1", ev_mb)
    must("kTypeLinearProofOutcomeReject", "AC1", ev_mb)

    # AC2 — soft path observes only, no force_reason override.
    must("production_defaults_active()", "AC2", ev_mb)
    must("AuditStrategy::Full", "AC2", ev_mb)
    must("kProofLiveGoalCountHintAuto", "AC2", ev_mb)

    # AC3 — quiet path two size reads, no extra atomics.
    must("pending == 0 && loc == 0", "AC3", impl)
    must("return SolveResult::SOLVED; // Quiet", "AC3", impl)

    # AC4 — lockless batch covered by composite_txn_commit drain.
    must("drain_pending_full_solve_before_commit", "AC4", tc)
    must("escalate_if_production(SolveResult::TIMEOUT", "AC4", impl)
    must("escalate_locality_slo_if_production", "AC4", impl)

    # AC5 — existing surfaces preserved (issue stamps + counters).
    must("kPendingFullSolveResidualIssue", "AC5", aud)
    must("pending_full_solve_residual", "AC5", aud)
    must("g_pending_full_solve_residual_observe_total", "AC5", aud)
    must("g_pending_full_solve_residual_escalate_total", "AC5", aud)
    must("g_pending_full_solve_residual_reject_total", "AC5", aud)

    # AC6 — tests extend existing suite (no new test_issue_3190.cpp), no
    # docs/design/3190-* per #1655, linter wired into build.py.
    must("ac3190_1_outermost_drain_production", "AC6", t)
    must("ac3190_2_outermost_drain_soft", "AC6", t)
    must("ac3190_3_quiet_zero_cost", "AC6", t)
    must("ac3190_4_lockless_batch_covered", "AC6", t)
    must("ac3190_5_existing_surfaces_preserved", "AC6", t)
    must("ac3190_6_source_and_linter", "AC6", t)
    must("check_residual_drain_outermost_stamp_3190", "AC6", build)

    if fails:
        print("FAIL #3190 residual_drain_outermost_stamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3190 residual_drain_outermost_stamp: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

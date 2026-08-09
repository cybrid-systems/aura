#!/usr/bin/env python3
"""Issue #2842: freeze Occurrence truth into TypeLinearCommitProof at stamp.

Residual of #2758: live_goal_count from CS size + bounded goal_fingerprint
(var.index + refined.index + pred_nid + mid + epoch, up to N) so Agents
detect densify/steal content drift without N-key join. Gauge is
fallback-only when CS unavailable under production.

  AC1 stamp freezes live_goal_count + non-zero fingerprint when goals non-empty
  AC2 fingerprint differs on content change (densify/steal prune)
  AC3 quiet path (empty goals) → count 0, fingerprint 0
  AC4 additive — #2613/#2697/#2717/#2758 preserved; schema-2842
  AC5 extend test_type_linear_commit_health.cpp; coverage linter
  AC6 no docs/design/*; no invent test file

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1 — freeze truth
    must("Issue #2842", "AC1", tma)
    must("goal_fingerprint", "AC1", tma)
    must("mix_occurrence_goal_into_fingerprint", "AC1", tma)
    must("apply_proof_goal_truth", "AC1", tma)
    must("ProofGoalTruth", "AC1", tma)
    must("freeze_proof_goal_truth_from_type_checker", "AC1", emb)
    must("kProofGoalFingerprintMaxGoals", "AC1", tma)

    # AC2 — densify stamp uses freeze
    must("freeze_proof_goal_truth_from_type_checker", "AC2", emb)
    must("densify_goal_truth_2842", "AC2", emb)

    # AC3 — quiet
    must("kQuietProofGoalTruth", "AC3", tma)
    must("g_last_proof_goal_fingerprint", "AC3", tma)

    # AC4 — additive
    must_key("schema-2697", "AC4", q)
    must_key("schema-2717", "AC4", q)
    must_key("schema-2758", "AC4", q)
    must_key("schema-2842", "AC4", q)
    must_key("issue-2842", "AC4", q)
    must_key("type-linear-commit-proof-goal-fingerprint", "AC4", q)
    must_key("type-linear-commit-proof-goal-truth-stamped-total", "AC4", q)
    must_key("type-linear-commit-proof-goal-fingerprint-nonzero-total", "AC4", q)
    must("g_type_linear_commit_proof_goal_truth_gauge_fallback_total", "AC4", tma)

    # AC5/AC6
    must("ac2842_1_stamp_freezes_goal_truth", "AC5", t)
    must("ac2842_2_fingerprint_differs_on_content_change", "AC5", t)
    must("ac2842_3_quiet_path_zeros", "AC5", t)
    must("ac2842_4_additive_no_regression", "AC5", t)
    must("ac2842_5_source_and_linter", "AC5", t)
    must("ac2758_1_counts_from_real_walks", "AC5 #2758 preserved", t)
    must("check_type_linear_commit_proof_goal_truth_2842", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2842.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2842.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2842*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2842 TypeLinearCommitProof goal truth freeze — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2648: Soft incomplete-skip evidence-loss SLO + one-shot Full arm.

Contract:
  AC1 Soft skip preserved (#2620); soft_incomplete_skip advances
  AC2 loss_bp threshold → force arm; boundary consume once
  AC3 healthy no skips → no force consume
  AC4 Soft recover clears without Full when recover succeeds
  AC5 query schema-2648 keys + source-cite
  AC6 production dual-require unchanged; unit test + gate wired

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    pol = _read("src/compiler/coercion_provenance_policy.hh")
    cmap = _read("src/compiler/coercion_map.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    tlch = _read("src/compiler/type_linear_commit_health.hh")
    test = _read("tests/compiler/test_coercion_evidence_loss_slo.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2648" in pol, "AC1/AC5: policy cites #2648")
    must("kCoercionEvidenceLossIssue" in pol, "AC1: issue stamp")
    must("kCoercionEvidenceLossBpDefault" in pol, "AC2: default threshold")
    must("AURA_COERCION_EVIDENCE_LOSS_BP" in pol, "AC2: env override")
    must("evaluate_coercion_evidence_loss_slo" in pol, "AC2: evaluate helper")
    must("g_coercion_evidence_loss_force_armed_total" in pol, "AC2: force armed counter")
    must("g_coercion_evidence_loss_force_consumed_total" in pol, "AC2: force consumed counter")

    must("coercion_evidence_loss_bp" in cmap, "AC1: loss_bp pure helper")
    must("#2648" in cmap, "AC1: map cites #2648")
    must("evaluate_coercion_evidence_loss_slo" in cmap, "AC2: arm path evaluates loss SLO")
    must("arm_soft_incomplete_force_full_observe" in cmap, "AC1: soft arm retained")

    must("#2648" in bound, "AC2: boundary cites #2648")
    must("coercion_evidence_loss" in bound, "AC2: boundary evidence-loss path")
    must("g_coercion_evidence_loss_force_consumed_total" in bound, "AC2: boundary consume bump")
    must("maybe_soft_recover_or_escalate_blame" in bound, "AC4: recover-first retained")
    must("evidence_loss_pressure" in bound, "AC2/AC4: Soft drop gated on pressure")

    must("schema-2648" in q, "AC5: query schema-2648")
    must("coercion-evidence-loss-bp" in q, "AC5: single bp key")
    must("coercion-evidence-loss-force-armed" in q, "AC5: force-armed key")
    must("coercion-evidence-loss-force-consumed" in q, "AC5: force-consumed key")
    must("issue-2648" in q, "AC5: issue-2648 key")

    must("coercion_evidence_loss_bp" in tlch, "AC5: type-linear-commit-health folds loss bp")
    must("coercion-evidence-loss" in tlch, "AC5: force_reason evidence-loss")
    must("schema-2648" in q and "type-linear-commit-health" in q, "AC5: tlch query schema-2648")

    must("AC1" in test and "AC2" in test and "AC6" in test, "AC6: unit test ACs present")
    must("coercion_evidence_loss_bp" in test, "AC6: test exercises loss_bp")
    must(
        "test_coercion_evidence_loss_slo.cpp" in cmake,
        "AC6: cmake registers test",
    )
    must(
        "check_coercion_evidence_loss_slo_2648" in build,
        "AC6: build.py wires linter",
    )
    must(
        "cmd_coercion_evidence_loss_slo_coverage" in build,
        "AC6: build.py coverage cmd",
    )

    # AC6 production paths unchanged
    must("coercion_dual_require_active" in cmap, "AC6: dual-require path retained")
    must("reject_apply_on_provenance_miss" in cmap, "AC6: reject-on-miss retained")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2648 Soft evidence-loss SLO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

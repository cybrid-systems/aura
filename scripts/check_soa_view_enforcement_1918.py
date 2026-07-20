#!/usr/bin/env python3
"""check_soa_view_enforcement_1918.py — Issue #1918 source-level gate.

Verifies SoAView / EDSL hot-path migration + pipeline DOD compliance:

  AC1: kSoaViewEnforcementPhase == 3, issue 1918
  AC2: HotPassDodCompliant concept exists
  AC3: Production wraps declare uses_soa_view
  AC4: EDSL record_*_soa_path helpers + counters
  AC5: Wired into apply_closure / query_matcher / mutate:rebind
  AC6: query stats schema-1918 on both surfaces
  AC7: test_soa_view_enforcement_1918.cpp exists
  AC8: cxx26_invariants phase >= 3
  AC9: pipeline check_pipeline_dod_compliance still called from run_pipeline
  AC10: edsl_column_access_ratio_bp + 90% target

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOA = ROOT / "src" / "compiler" / "soa_view.ixx"
PASS = ROOT / "src" / "compiler" / "pass_manager.ixx"
CONCEPTS = ROOT / "src" / "core" / "concept_constraints.ixx"
CXX26 = ROOT / "src" / "core" / "cxx26_invariants.ixx"
EVAL_FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
MATCHER = ROOT / "src" / "compiler" / "query_matcher.cpp"
MUTATE = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
OBS = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
STDREV = ROOT / "src" / "compiler" / "evaluator_primitives_stdlib_review.cpp"
TEST = ROOT / "tests" / "test_soa_view_enforcement_1918.cpp"


def main() -> int:
    failures: list[str] = []
    soa = SOA.read_text(encoding="utf-8", errors="replace")
    pm = PASS.read_text(encoding="utf-8", errors="replace")
    cc = CONCEPTS.read_text(encoding="utf-8", errors="replace")
    inv = CXX26.read_text(encoding="utf-8", errors="replace")
    flat = EVAL_FLAT.read_text(encoding="utf-8", errors="replace")
    matcher = MATCHER.read_text(encoding="utf-8", errors="replace")
    mutate = MUTATE.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    stdrev = STDREV.read_text(encoding="utf-8", errors="replace")

    if "kSoaViewEnforcementPhase = 3" not in soa and "kSoaViewEnforcementPhase=3" not in soa:
        failures.append("AC1: kSoaViewEnforcementPhase != 3")
    if "1918" not in soa[soa.find("kSoaViewEnforcementIssue") : soa.find("kSoaViewEnforcementIssue") + 60]:
        failures.append("AC1: kSoaViewEnforcementIssue not 1918")

    if "HotPassDodCompliant" not in cc:
        failures.append("AC2: HotPassDodCompliant missing")

    if "uses_soa_view" not in pm:
        failures.append("AC3: uses_soa_view missing entirely")
    if pm.count("uses_soa_view") < 6:
        failures.append("AC3: too few uses_soa_view declarations")

    for name in (
        "record_edsl_matcher_soa_path",
        "record_edsl_apply_soa_path",
        "record_edsl_mutate_soa_path",
        "record_edsl_children_soa_path",
        "g_edsl_matcher_soa_hits",
        "edsl_column_access_ratio_bp",
        "kEdslSoaColumnAccessTargetBp",
    ):
        if name not in soa:
            failures.append(f"AC4: soa_view missing {name}")

    if "record_edsl_apply_soa_path" not in flat:
        failures.append("AC5: apply_closure not wired")
    if "record_edsl_matcher_soa_path" not in matcher:
        failures.append("AC5: query_matcher not wired")
    if "record_edsl_mutate_soa_path" not in mutate:
        failures.append("AC5: mutate:rebind not wired")

    if "schema-1918" not in obs or "schema-1918" not in stdrev:
        failures.append("AC6: schema-1918 missing from stats surfaces")
    if "soa-view-concept-enforced" not in stdrev:
        failures.append("AC6: production-sweep missing soa-view-concept-enforced")

    if not TEST.is_file():
        failures.append("AC7: test missing")

    if "kSoaViewEnforcementPhaseConsteval = 3" not in inv:
        failures.append("AC8: cxx26 phase not 3")

    if "check_pipeline_dod_compliance" not in pm:
        failures.append("AC9: check_pipeline_dod_compliance missing")
    if "run_pipeline" not in pm or "check_pipeline_dod_compliance<Passes" not in pm:
        failures.append("AC9: run_pipeline does not call pack check")

    if "9000" not in soa or "edsl_column_access_ratio_bp" not in soa:
        failures.append("AC10: 90% target / ratio helper missing")

    if failures:
        print("check_soa_view_enforcement_1918: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_soa_view_enforcement_1918: 10/10 ACs satisfied ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())

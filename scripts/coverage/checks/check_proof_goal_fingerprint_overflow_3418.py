#!/usr/bin/env python3
"""Issue #3418: kProofGoalFingerprintMaxGoals=16 overflow is silent-green.

Prefix mix of 16 Occurrence goals does not see tail drift. Production/Full
refuse green stamp / persist (force_reason 16). Soft mixes 16, observe only.
No new query key. Residual of #2842/#3170.

Contract:
  AC1 expose overflow (proof field or occurrence_consistent=false)
  AC2 Production/Full refuse green; Soft observe
  AC3 persist fail-closed on overflow even if 16-prefix matches
  AC4 live_goal_count already published; stamp must not ignore n>16
  AC5 extend persist-rehydrate + commit-health; linter after #3170; no invent

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tchk = _read("src/compiler/type_checker.ixx")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    treh = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    thlth = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kProofGoalFingerprintOverflowIssue = 3418", "AC1 stamp", tma)
    must("kProofGoalFingerprintMaxGoals = 16", "AC1 cap stays 16", tma)
    must("fingerprint_overflow", "AC1 ProofGoalTruth field", tma)
    must("occurrence_consistent = false", "AC1 occurrence_consistent", tma)
    must("reject_fingerprint_cap_overflow", "AC1 helper", tma)

    fn = tma.find("inline void reject_fingerprint_cap_overflow")
    fn_win = tma[fn : fn + 1600] if fn >= 0 else ""
    must("production_defaults_active()", "AC2 production gate", fn_win)
    must("AuditStrategy::Full", "AC2 Full gate", fn_win)
    must("force_reason_code = 16", "AC2 force_reason 16", fn_win)
    must("kTypeLinearProofOutcomeReject", "AC2 Reject", fn_win)
    must("ac3418_fingerprint_cap_overflow_rejects", "AC2 persist-rehydrate test", treh)
    must("Soft overflow still mixes 16", "AC2 Soft fixture", treh)

    must("Issue #3418", "AC3 persist cite", emb)
    persist = emb.find('extern "C" void aura_outermost_success_persist_occurrence')
    persist_win = emb[persist : persist + 4200] if persist >= 0 else ""
    must("fingerprint_overflow", "AC3 persist overflow", persist_win)
    must("kProofGoalFingerprintMaxGoals", "AC3 persist cap", persist_win)
    must("force_reason=*/16", "AC3 persist force 16", persist_win)

    must("live_goal_count > kProofGoalFingerprintMaxGoals", "AC4 stamp consults count", tma)
    must("live_goal_count published as 17", "AC4 fixture", treh)
    must("fingerprint_overflow", "AC4 freeze", emb)
    must("kProofGoalFingerprintMaxGoals", "AC4 health overflow", tchk)
    must("fingerprint_overflow", "AC4 steal", steal)

    if "schema-3418" in q or "schema-3418" in tma:
        fails.append("AC4: new schema-3418 query key (forbidden)")
    if "g_3418_" in tma or "g_3418_" in emb:
        fails.append("AC4: new g_3418_* counter (forbidden)")

    must("check_proof_goal_fingerprint_overflow_3418", "AC5 build.py", build)
    must("check_occurrence_persist_fingerprint_3170", "AC5 predecessor #3170", build)
    i3346 = build.find("check_stamp_last_look_densify_steal_abort_3346")
    i3418 = build.find("check_proof_goal_fingerprint_overflow_3418")
    if i3346 < 0 or i3418 < 0 or i3418 < i3346:
        fails.append("AC5: #3418 linter must run after #3346 fingerprint last-look")
    must("3418", "AC5 commit-health suite", thlth)
    must("ac3418_source_and_linter", "AC5 persist-rehydrate linter AC", treh)
    if (ROOT / "tests" / "compiler" / "test_issue_3418.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3418.cpp present (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3418.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3418.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3418-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_proof_goal_fingerprint_overflow_3418: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3418 fingerprint cap overflow — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

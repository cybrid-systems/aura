#!/usr/bin/env python3
"""Issue #3431: unstaged expected_fp==0 skips #3170 guard under Production.

#3170 / #3376 freeze Occurrence persist only when live fingerprint matches
the staged expected snapshot. The Production conjunct required
expected != 0, so never-staged (always 0) fell through to
maybe_persist_occurrence_snapshot. Production/Full now abort when
expected==0 and live goals are nonempty. Soft keeps 0==0 skip.
Reuse mismatch + force_reason 16. No new query key.

Contract:
  AC1 Production + expected_fp==0 + nonempty live goals → no persist
      write; proof Reject; grant false
  AC2 Production + staged expected matches live → persist unchanged
      (#3170 / #3376 needle kept)
  AC3 Soft / Off: expected 0 does not bump mismatch
  AC4 #3406 recover-fail clear still required (sibling; do not close)
  AC5 no docs/design/*; no test_issue_*.cpp; linter after #3170/#3418

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    t = _read("tests/compiler/test_outermost_persist_fail_closed.cpp")
    build = _read("build.py")
    lint3170 = _read("scripts/coverage/checks/check_occurrence_persist_fingerprint_3170.py")
    lint3406 = _read("scripts/check_recover_fail_clear_persist_3406.py")

    start = emb.find('extern "C" void aura_outermost_success_persist_occurrence(')
    persist = emb.find("maybe_persist_occurrence_snapshot", start) if start >= 0 else -1
    win = emb[start:persist] if start >= 0 and persist > start else ""

    must("kOccurrenceUnstagedExpectedFpIssue = 3431", "AC1 stamp", tma)
    must("Issue #3431", "AC1 cite", win)
    must("expected == 0", "AC1 unstaged", win)
    must("live_goal_count > 0", "AC1 nonempty live", win)
    must("clear_occurrence_persist_buffer(tc)", "AC1 no write", win)
    must("kTypeLinearProofOutcomeReject", "AC1 Reject", win)
    must("clear_type_export_authority()", "AC1 grant false", win)
    must("force_reason=*/16", "AC1 reuse 16", win)
    must("bump_occurrence_persist_fingerprint_mismatch", "AC1 reuse mismatch", win)
    must("3431 AC1", "AC1 test", t)

    needle = (
        "if (aura::compiler::typed_audit::production_defaults_active() &&\n"
        "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
        "        live_fp != ev->expected_occurrence_snapshot_fp()) {"
    )
    if needle not in emb:
        fails.append("AC2: #3170 staged-mismatch needle missing")
    must("Issue #3376", "AC2 #3376 kept", emb)
    must("3431 AC2", "AC2 test", t)

    must("Soft keeps expected==0 skip", "AC3 Soft skip", emb)
    must("3431 AC3", "AC3 test", t)

    must("Issue #3406", "AC4 #3406 kept", emb)
    must("if (!tc->ensure_occurrence_commit_or_recover())", "AC4 recover-fail", emb)
    must("3406", "AC4 3406 linter kept", lint3406)
    must("3431 AC4", "AC4 test", t)

    must("check_occurrence_unstaged_expected_fp_3431", "AC5 build.py", build)
    must("3431 AC5", "AC5 test", t)
    must("check_occurrence_persist_fingerprint_3170", "AC5 3170 lineage", build)
    must("3170", "AC5 3170 linter kept", lint3170)
    prev3418 = build.find("check_proof_goal_fingerprint_overflow_3418")
    ours = build.find("check_occurrence_unstaged_expected_fp_3431")
    if prev3418 < 0 or ours < 0 or ours < prev3418:
        fails.append("AC5: #3431 linter must run after #3418")
    if "schema-3431" in emb or "schema-3431" in tma:
        fails.append("AC5: new schema-3431 query key")
    if "g_3431_" in emb or "g_3431_" in tma:
        fails.append("AC5: new g_3431_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3431.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3431.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3431.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3431.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3431-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3431 occurrence_unstaged_expected_fp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3431 occurrence_unstaged_expected_fp: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

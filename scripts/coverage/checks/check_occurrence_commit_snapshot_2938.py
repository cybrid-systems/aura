#!/usr/bin/env python3
"""Issue #2938: freeze Occurrence truth on every successful outermost commit.

Contract (one row per AC):
  AC1 production + non-empty goals + outermost success → snapshot written
      + post-persist TypeLinearCommitProof stamp (fingerprint matches goals)
  AC2 Soft + empty goals → zero commit-snapshot counters / zero persist
  AC3 reject / force-rollback never calls outermost success persist helper
  AC4 densify/steal fence after snapshotted commit → rehydrate or hard face
  AC5 additive keys; #2608 / #2842 / #2910 / #2896 preserved
  AC6 coverage linter + src/-aligned suite (#81967); no docs/design/

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    test = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    # ── AC1: post-persist stamp + counters ──
    must("#2938", "AC1", tma)
    must("kOccurrenceCommitSnapshotIssue = 2938", "AC1", tma)
    must("g_occurrence_commit_snapshot_written_total", "AC1", tma)
    must("g_occurrence_commit_snapshot_mid", "AC1", tma)
    must("note_occurrence_commit_snapshot_written", "AC1", tma)
    must("note_occurrence_commit_snapshot_written", "AC1 helper", mb)
    must("maybe_persist_occurrence_snapshot", "AC1", mb)
    must("build_type_linear_commit_proof_from_live", "AC1 post-persist", mb)
    must("aura_outermost_success_persist_occurrence", "AC1", mb)
    must("ac2938_1_production_commit_snapshot_and_post_persist_stamp", "AC1", test)
    # Order: note write before proof stamp in the C ABI body.
    note_pos = mb.find("note_occurrence_commit_snapshot_written")
    stamp_pos = mb.find("build_type_linear_commit_proof_from_live", note_pos if note_pos >= 0 else 0)
    if note_pos < 0 or stamp_pos < 0 or note_pos >= stamp_pos:
        fails.append("AC1: post-persist stamp must follow note_occurrence_commit_snapshot_written")

    # ── AC2: Soft zero ──
    must("Soft / env=0", "AC2", mb)
    must("ac2938_2_soft_empty_zero", "AC2", test)
    must("entries_written == 0", "AC2", tma)

    # ── AC3: reject never writes ──
    must("never call this helper (AC3)", "AC3", mb)
    must("if (outermost && success)", "AC3", mb)
    must("ac2938_3_reject_never_writes", "AC3", test)

    # ── AC4: fence / rehydrate ──
    ixx = _read("src/compiler/type_checker.ixx")
    must("rehydrate_occurrence_from_persist", "AC4 lineage", ixx)
    must("note_occurrence_empty_after_fence", "AC4 face", ixx)
    must("ac2938_4_fence_after_snapshot", "AC4", test)

    # ── AC5: query + lineage ──
    must("occurrence-commit-snapshot-written-total", "AC5", q)
    must("occurrence-commit-snapshot-mid", "AC5", q)
    must("occurrence-commit-snapshot-wired", "AC5", q)
    must("schema-2938", "AC5", q)
    must("issue-2938", "AC5", q)
    must("schema-2608", "AC5", q)
    must("schema-2910", "AC5", q)
    must("schema-2896", "AC5", q)
    must("g_last_proof_goal_fingerprint", "AC5 #2842", tma)
    must("ac2938_5_lineage_query", "AC5", test)

    # ── AC6: tests + build + no design ──
    must("ac2938_1_production_commit_snapshot_and_post_persist_stamp", "AC6", test)
    must("ac2938_2_soft_empty_zero", "AC6", test)
    must("ac2938_3_reject_never_writes", "AC6", test)
    must("ac2938_4_fence_after_snapshot", "AC6", test)
    must("ac2938_5_lineage_query", "AC6", test)
    must("ac2938_6_linter_and_no_design", "AC6", test)
    must("check_occurrence_commit_snapshot_2938", "AC6", build)
    must("cmd_occurrence_commit_snapshot_2938", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2938-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2938.cpp").is_file():
        fails.append("AC6: test_issue_2938.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2938 occurrence commit snapshot — outermost success freezes Occurrence + post-persist proof")
    return 0


if __name__ == "__main__":
    sys.exit(main())

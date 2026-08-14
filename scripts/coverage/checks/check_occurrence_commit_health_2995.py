#!/usr/bin/env python3
"""Issue #2995: unified OccurrenceCommitHealth + single-shot recover.

Contract (one row per AC):
  AC1 Soft + empty / no faces: evaluate is pure loads; no persist; no recover
  AC2 production + empty_after_fence / cone_outside / refined_drift →
      needs_recover; ensure runs existing try_occurrence_* once
  AC3 recover fail: existing force_reason (no silent green)
  AC4 outermost success still sole persist writer (#2938); fingerprint matches
  AC5 steal/densify fence: health after rehydrate; same ensure entry (#2910 order)
  AC6 query bitmask + goals_live + persist_size + needs_recover + recovered_ok
  AC7 extend persist-rehydrate + commit-health suites; no docs/design / invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/type_checker.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    q = read_query_prims()
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("struct OccurrenceCommitHealth", "AC1", ixx)
    must("evaluate_occurrence_commit_health", "AC1", ixx)
    must("ac2995_1_soft_empty_pure_loads", "AC1", persist)

    must("ensure_occurrence_commit_or_recover", "AC2", ixx)
    must("try_occurrence_hard_face_full_solve_recover", "AC2", ixx)
    must("ac2995_2_production_face_one_shot_recover", "AC2", persist)
    # No second recover solver — ensure must call the existing trampoline.
    if ixx.count("try_occurrence_hard_face_full_solve_recover") < 2:
        fails.append("AC2: ensure must call existing try_occurrence_hard_face_full_solve_recover")

    must("ac2995_3_recover_fail_keeps_reject", "AC3", health)
    must("force_reason", "AC3", health)

    must("maybe_persist_occurrence_snapshot", "AC4", mb)
    must("ensure_occurrence_commit_or_recover", "AC4", mb)
    persist_pos = mb.find("maybe_persist_occurrence_snapshot")
    stamp_pos = mb.find("build_type_linear_commit_proof_from_live", persist_pos if persist_pos >= 0 else 0)
    ens_pos = mb.find("ensure_occurrence_commit_or_recover", stamp_pos if stamp_pos >= 0 else 0)
    if persist_pos < 0 or stamp_pos < 0 or ens_pos < 0 or not (persist_pos < stamp_pos < ens_pos):
        fails.append("AC4: outermost order must be persist → #2938 stamp → #2995 ensure")
    must("ac2995_4_fingerprint_after_persist", "AC4", persist)
    must("#2938", "AC4 comment", mb)

    reh = ixx.find("rehydrate_occurrence_from_persist")
    ens = ixx.find("ensure_occurrence_commit_or_recover", reh if reh >= 0 else 0)
    if reh < 0 or ens < 0 or reh >= ens:
        fails.append("AC5: fence ensure must follow #2910 rehydrate")
    must("#2910", "AC5 order comment", ixx)
    must("ac2995_5_fence_same_ensure", "AC5", persist)

    must_key("schema-2995", "AC6", q)
    must_key("occurrence-commit-health-faces", "AC6", q)
    must_key("occurrence-commit-health-goals-live", "AC6", q)
    must_key("occurrence-commit-health-persist-size", "AC6", q)
    must_key("occurrence-commit-health-needs-recover", "AC6", q)
    must_key("occurrence-commit-health-recovered-ok", "AC6", q)
    must("kOccurrenceCommitHealthIssue", "AC6", tma)
    must("ac2995_6_query_keys", "AC6", persist)
    must("ac2995_6_health_query_keys", "AC6", health)

    must("ac2995_7_source_cite", "AC7", persist)
    must("check_occurrence_commit_health_2995", "AC7", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2995.cpp").is_file():
        fails.append("AC7: test_issue_2995.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2995-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2995 OccurrenceCommitHealth — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

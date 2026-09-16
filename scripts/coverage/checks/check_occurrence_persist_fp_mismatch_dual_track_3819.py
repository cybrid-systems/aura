#!/usr/bin/env python3
"""Issue #3819: Occurrence persist fingerprint mismatch reject is prod||Full.

Outermost live_fp ≠ expected_occurrence_snapshot_fp reject was gated only on
production_defaults_active(). Align with #3788/#3794/#3431/#3556 dual-track
via production_hard_face_active() (production ∥ Full). Soft/Off unchanged.

Contract (one row per AC):
  AC1  emb mismatch uses production_hard_face_active + reject stamp
  AC2  Soft/Off: Soft/Off unchanged cite; no Soft-only hard refuse
  AC3  Soak: Full-without-prod hard-face + early-return needle in suite
  AC4  Tests extend named suites; no invent / docs/design / new query key

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    text = p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""
    return " ".join(text.split())


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        n = " ".join(n.split())
        hay = " ".join(hay.split())
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t_persist = _read("tests/compiler/test_outermost_persist_fail_closed.cpp")
    t_lin = _read("tests/compiler/test_linear_enforce_production_defaults.cpp")
    t_health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    t_rehy = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")
    tma = _read("src/compiler/typed_mutation_audit.h")

    must("Issue #3819", "AC1 cite", emb)
    fn = emb.find('extern "C" void aura_outermost_success_persist_occurrence(')
    after = emb[fn:] if fn >= 0 else ""
    cite = after.find("Issue #3819")
    win = after[cite : cite + 2400] if cite >= 0 else ""
    must("production_hard_face_active()", "AC1 hard-face", win)
    must("expected_occurrence_snapshot_fp() != 0", "AC1 staged", win)
    must("live_fp != ev->expected_occurrence_snapshot_fp()", "AC1 mismatch", win)
    must("kTypeLinearProofOutcomeReject", "AC1 reject outcome", win)
    must("bump_occurrence_persist_fingerprint_mismatch", "AC1 bump", win)
    # Must not leave the old prod-only gate on the mismatch needle.
    old = (
        "if (aura::compiler::typed_audit::production_defaults_active() &&\n"
        "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
        "        live_fp != ev->expected_occurrence_snapshot_fp()) {"
    )
    if old in emb:
        fails.append("AC1: prod-only mismatch gate still present")
    must(
        "if (aura::compiler::typed_audit::production_hard_face_active() &&\n"
        "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
        "        live_fp != ev->expected_occurrence_snapshot_fp()) {",
        "AC1 hard-face needle",
        emb,
    )
    must("production_hard_face_active()", "AC1 helper def", tma)
    must("get_strategy() == AuditStrategy::Full", "AC1 Full face in hard-face", tma)

    must("Soft/Off unchanged", "AC2 Soft cite", win)
    must("3819 AC2", "AC2 Soft suite", t_persist)

    must("3819 soak", "AC3 soak", t_persist)
    must("Full-without-prod", "AC3 Full-without-prod", t_persist)

    must("3819", "AC4 persist suite", t_persist)
    must("3819", "AC4 linear suite", t_lin)
    must("3819", "AC4 health suite", t_health)
    must("production_hard_face_active()", "AC4 rehydrate needle", t_rehy)
    must("check_occurrence_persist_fp_mismatch_dual_track_3819", "AC4 build", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3819.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3819.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3819.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3819.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3819-*")):
            fails.append(f"AC4: docs/design/{f.name} present")
    if "schema-3819" in emb:
        fails.append("AC4: schema-3819 query key present")

    if fails:
        for e in fails:
            print(f"FAIL: {e}", file=sys.stderr)
        return 1
    print("OK: Issue #3819 fingerprint mismatch dual-track (prod||Full)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

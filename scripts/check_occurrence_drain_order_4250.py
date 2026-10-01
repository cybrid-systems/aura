#!/usr/bin/env python3
# scripts/check_occurrence_drain_order_4250.py -- Issue #4250 source-cite gate.
#
# AC1: within aura_outermost_success_persist_occurrence the pending_full_solve
#      drain (drain_pending_full_solve_before_commit) is reached BEFORE the
#      Occurrence persist write (maybe_persist_occurrence_snapshot) so the
#      frozen snapshot observes the drained CS and matches the post-drain
#      TypeLinearCommitProof (#3190 ordering residual).
# AC2: freeze -> note_occurrence_commit_snapshot_written -> proof stamp order
#      retained (#2938 / #2995 / #3004 lineage unchanged).
# AC3: the drain hard-reject arm still clears the Occurrence persist buffer,
#      bumps the #3170 mismatch counter, and the reordered site cites #4250.
# AC4: Soft/Off zero-cost preserved: the drain stays production/Full gated;
#      no new metrics field / schema-4250 / docs/design/4250-* /
#      tests/**/test_issue_4250.cpp.
# AC5: the src/-aligned suite hosts the new ACs
#      (tests/compiler/test_occurrence_persist_rehydrate.cpp) and build.py +
#      scripts/coverage/root_check_allowlist.txt wire this linter.
#
# Exit 0 = all rows satisfied.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MB = "src/compiler/evaluator_mutation_boundary.cpp"
REH = "tests/compiler/test_occurrence_persist_rehydrate.cpp"
BUILD = "build.py"
OBS = "src/compiler/observability_metrics.h"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

HELPER = 'extern "C" void aura_outermost_success_persist_occurrence('
PERSIST = "maybe_persist_occurrence_snapshot"
DRAIN = "drain_pending_full_solve_before_commit"
NOTE = "note_occurrence_commit_snapshot_written"
STAMP = "build_type_linear_commit_proof_from_live"
GRANT = "grant_type_export_authority"
SITE = "Issue #4250"
REH_AC1 = "4250 AC1: drain_pending_full_solve_before_commit precedes "


def _rows(mb: str, reh: str, build: str, obs: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(cond: bool, label: str) -> None:
        if not cond:
            fails.append(label)

    helper = mb.find(HELPER)
    persist = mb.find(PERSIST, helper) if helper >= 0 else -1
    drain = mb.find(DRAIN, helper) if helper >= 0 else -1

    # AC1 -- drain before the Occurrence freeze inside the persist helper.
    must(helper >= 0, "AC1: persist helper definition not found")
    must(persist >= 0, "AC1: maybe_persist_occurrence_snapshot not found in helper")
    must(drain >= 0, "AC1: drain_pending_full_solve_before_commit not found in helper")
    must(
        drain >= 0 and persist >= 0 and drain < persist,
        "AC1: drain must precede maybe_persist_occurrence_snapshot (freeze after drain)",
    )

    if helper >= 0:
        end = mb.find(GRANT, persist if persist >= 0 else helper)
        win = mb[helper : end if end >= 0 else len(mb)]
    else:
        win = ""
    note = win.find(NOTE)
    stamp = win.find(STAMP, note if note >= 0 else 0)

    # AC2 -- freeze -> note -> stamp order retained.
    must(note >= 0, "AC2: note_occurrence_commit_snapshot_written missing in helper")
    must(stamp >= 0, "AC2: proof stamp missing after note")
    must(note >= 0 and stamp >= 0 and note < stamp, "AC2: note -> proof stamp order broken")

    drain_win = win[: note if note >= 0 else len(win)]

    # AC3 -- reject arm retains clear + mismatch bump and cites #4250.
    must(
        "clear_occurrence_persist_buffer" in drain_win, "AC3: drain reject arm must clear the Occurrence persist buffer"
    )
    must(
        "bump_occurrence_persist_fingerprint_mismatch" in drain_win,
        "AC3: drain reject arm must bump the #3170 mismatch counter",
    )
    must(SITE in win, "AC3: reordered site must cite #4250")

    # AC4 -- Soft/Off zero-cost + no new surface.
    must(
        "production_defaults_active()" in drain_win and "AuditStrategy::Full" in drain_win,
        "AC4: drain reject must stay production/Full gated (Soft observe-only)",
    )
    must("4250" not in obs, "AC4: no new metrics field may cite 4250")
    for rel in ("tests/compiler/test_issue_4250.cpp", "tests/issues/test_issue_4250.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden {rel} exists (#81934)")
    if any(p.name.find("4250-") >= 0 for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC4: forbidden docs/design/*4250-* exists (#1655)")

    # AC5 -- suite hosts the ACs; linter wired.
    must(REH_AC1 in reh, "AC5: test_occurrence_persist_rehydrate hosts #4250 AC1")
    must("check_occurrence_drain_order_4250" in build, "AC5: build.py must wire check_occurrence_drain_order_4250")
    must("check_occurrence_drain_order_4250.py" in allow, "AC5: root_check_allowlist.txt must list the linter")

    return fails


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        good_mb = (
            HELPER
            + "\n"
            + DRAIN
            + "\n"
            + "clear_occurrence_persist_buffer\n"
            + "bump_occurrence_persist_fingerprint_mismatch\n"
            + "production_defaults_active()\nAuditStrategy::Full\n"
            + SITE
            + "\n"
            + PERSIST
            + "\n"
            + NOTE
            + "\n"
            + STAMP
            + "\n"
            + GRANT
            + "\n"
        )
        good_fails = _rows(
            good_mb,
            REH_AC1 + "maybe_persist_occurrence_snapshot in the helper\n",
            "check_occurrence_drain_order_4250",
            "clean-obs",
            "check_occurrence_drain_order_4250.py\n",
        )
        if good_fails:
            for f in good_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "4250", "")
        if len(neg_fails) < 8:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(MB), _read(REH), _read(BUILD), _read(OBS), _read(ALLOW))
    if fails:
        print("FAIL #4250 occurrence_drain_order:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4250 occurrence_drain_order: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

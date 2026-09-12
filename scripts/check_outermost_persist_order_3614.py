#!/usr/bin/env python3
# scripts/check_outermost_persist_order_3614.py -- Issue #3614 source-cite gate.
#
# AC1: the #3614 pre-persist gate (linear walk + pending_full_solve drain,
#      production/Full-gated) sits BEFORE the outermost persist call; the
#      deny arm reuses the #3472 un-stamp set (proof / persist / grant).
# AC2: order gate → persist → #3440 consume → #3472 belt-and-suspenders →
#      exit; sole snapshot writer remains the persist helper (exactly one
#      note_occurrence_commit_snapshot_written call); still exactly 3
#      #3158 restore sites; no second restore helper.
# AC3: #3440 note + #3545 coercion journal undo retained inside the
#      persist-reject paths.
# AC4: Soft/Off zero-cost preserved (gate face-cited in AC1); no new
#      metrics field, no schema-3614 query key, no docs/design/*3614-*,
#      no tests/**/test_issue_3614.cpp.
# AC5: ac3614_* rows live in test_type_linear_commit_health.cpp (runtime
#      linear-deny fixture + written_total unchanged) and the #3614
#      source-cite section in test_occurrence_persist_rehydrate.cpp;
#      #3472 / #3556 ACs retained unchanged.
# AC6: build.py wires this linter + scripts/coverage/root_check_allowlist.txt.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MB = "src/compiler/evaluator_mutation_boundary.cpp"
HEALTH = "tests/compiler/test_type_linear_commit_health.cpp"
REH = "tests/compiler/test_occurrence_persist_rehydrate.cpp"
BUILD = "build.py"
QUERY = "src/compiler/evaluator_primitives_obs_eval.cpp"
OBS = "src/compiler/observability_metrics.h"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

SITE = "Issue #3614"
PERSIST_CALL = "aura_outermost_success_persist_occurrence(ev_"
CONSUME = "consume_outermost_persist_reject_needs_restore()"
BELT = "Issue #3472"
EXIT = "ev_->exit_mutation_boundary(success)"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _count(hay: str, needle: str) -> int:
    n = 0
    p = hay.find(needle)
    while p != -1:
        n += 1
        p = hay.find(needle, p + 1)
    return n


def _rows(mb: str, health: str, reh: str, build: str, query: str, obs: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    gate = mb.find(SITE)
    persist_call = mb.find(PERSIST_CALL)
    consume = mb.find(CONSUME)
    belt = mb.find(BELT)
    exit_pos = mb.find(EXIT)

    # AC1: pre-persist gate with walk + drain + production/Full face +
    # deny un-stamp set, all inside the gate window.
    if gate == -1:
        fails.append("AC1: gate cite missing")
    if persist_call == -1:
        fails.append("AC1: persist call site missing")
    if gate != -1 and persist_call != -1 and gate < persist_call:
        win = mb[gate:persist_call]
        must("enforce_linear_boundary_consistency", "AC1 gate walk", win)
        must("drain_pending_full_solve_before_commit", "AC1 gate drain", win)
        must("production_defaults_active()", "AC1 gate production face", win)
        must("get_strategy() == typed_audit::AuditStrategy::Full", "AC1 gate Full face", win)
        must("clear_type_linear_commit_proof_on_abort()", "AC1 deny clear proof", win)
        must("kTypeLinearProofOutcomeReject", "AC1 deny publish", win)
        must("aura_clear_occurrence_persist_buffer(ev_)", "AC1 deny clear persist", win)
        must("clear_type_export_authority", "AC1 deny clear grant", win)
    else:
        fails.append("AC1: gate does not precede the persist call")

    # AC2: order gate → persist → consume → belt → exit; sole writer;
    # exactly 3 #3158 restore sites; no second restore helper.
    if not (
        gate != -1
        and persist_call != -1
        and consume != -1
        and belt != -1
        and exit_pos != -1
        and gate < persist_call < consume < belt < exit_pos
    ):
        fails.append("AC2: order gate → persist → consume → #3472 belt → exit broken")
    if _count(mb, "note_occurrence_commit_snapshot_written(") != 1:
        fails.append("AC2: sole snapshot writer count != 1 (persist helper)")
    if _count(mb, "restore_or_clear_occurrence_to_entry(") != 4:
        fails.append("AC2: #3158 restore site count != 4 (3 abort + persist-reject #3687)")
    must_not("abort_restore_3614", "AC2 no second restore", mb)
    must_not("abort_restore_dual_topology_3614", "AC2 no second restore (full)", mb)

    # AC3: #3440 note + #3545 undo retained.
    must("note_outermost_persist_reject_needs_restore", "AC3 3440 note", mb)
    must("undo_apply_coercion_map_recent", "AC3 3545 undo", mb)

    # AC4: Soft zero-cost / no new surface.
    must_not("3614", "AC4 no new metrics field", obs)
    must_not("schema-3614", "AC4 no new query key", query)
    for rel in ("tests/issues/test_issue_3614.cpp", "tests/compiler/test_issue_3614.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden {rel} exists (#81934)")
    if any(p.name.find("3614-") >= 0 for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC4: forbidden docs/design/*3614-* exists (#1655)")

    # AC5: test rows present; #3472 / #3556 ACs retained.
    must("ac3614_1_linear_deny_pre_persist_no_write", "AC5 runtime deny AC", health)
    must("occurrence_commit_snapshot_written_total_v_read", "AC5 written_total read", health)
    must("ac3614_3_soft_zero_cost_no_flip", "AC5 Soft AC", health)
    must("ac3614_4_persist_reject_and_happy_unchanged", "AC5 happy AC", health)
    must(SITE, "AC5 health cite", health)
    must("ac3472_1_persist_green_phase1_deny", "AC5 3472 runtime retained", health)
    must("ac3472_2_persist_reject_unchanged", "AC5 3472 order retained", health)
    must(SITE, "AC5 reh cite", reh)
    must("3614 AC1", "AC5 reh source-cite rows", reh)
    must("3556 AC1", "AC5 3556 rows retained", reh)

    # AC6: build.py wiring + root_check_allowlist entry.
    must("check_outermost_persist_order_3614", "AC6 build.py wiring", build)
    must("check_outermost_persist_order_3614.py", "AC6 allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_mb = (
            SITE
            + "\nenforce_linear_boundary_consistency\ndrain_pending_full_solve_before_commit\n"
            + "production_defaults_active()\n"
            + "get_strategy() == typed_audit::AuditStrategy::Full\n"
            + "clear_type_linear_commit_proof_on_abort()\nkTypeLinearProofOutcomeReject\n"
            + "aura_clear_occurrence_persist_buffer(ev_)\nclear_type_export_authority\n"
            + PERSIST_CALL
            + "\n"
            + CONSUME
            + "\n"
            + BELT
            + "\n"
            + EXIT
            + "\n"
            + "note_occurrence_commit_snapshot_written(\n"
            + "restore_or_clear_occurrence_to_entry(a)restore_or_clear_occurrence_to_entry(b)"
            + "restore_or_clear_occurrence_to_entry(c)restore_or_clear_occurrence_to_entry(d)\n"
            + "note_outermost_persist_reject_needs_restore\n"
            + "undo_apply_coercion_map_recent\n"
        )
        sample_health = (
            "ac3614_1_linear_deny_pre_persist_no_write\n"
            "occurrence_commit_snapshot_written_total_v_read\n"
            "ac3614_3_soft_zero_cost_no_flip\nac3614_4_persist_reject_and_happy_unchanged\n"
            + SITE
            + "\nac3472_1_persist_green_phase1_deny\nac3472_2_persist_reject_unchanged\n"
        )
        sample_reh = SITE + "\n3614 AC1\n3556 AC1\n"
        ok_fails = _rows(
            sample_mb,
            sample_health,
            sample_reh,
            "check_outermost_persist_order_3614",
            "clean",
            "clean",
            "check_outermost_persist_order_3614.py",
        )
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "", "", "", "")
        if len(neg_fails) < 10:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(MB), _read(HEALTH), _read(REH), _read(BUILD), _read(QUERY), _read(OBS), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("outermost persist order (#3614) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

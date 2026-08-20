#!/usr/bin/env python3
"""Issue #3185: densify-entry LCP consult (steal×GC residual).

Residual of #2888 / #2957 / #3055 densify/steal arms. Production code
review 2026-08-20 (HEAD `ae1e2de`) found that densify ENTRY side never
consults `last_lifetime_consistency_proof().would_allow_commit` —
stamp-on-exit works (#2888) but entry didn't check last proof before
relocating. Concurrent fiber steal arm could publish reject after the
entry already started relocating. #3185 closes the gap:

  - Phase-5 main densify entry: consult + pin_contract_held=false on block
  - Optional one-shot Moving densify (recover_moving_sticky_densify_off):
    mirror the same surface
  - Soft / Off: zero-cost (single atomic load + early-return path guarded
    by `production_defaults_active() || get_strategy() == Full`)
  - evaluator_gc.cpp compact_sweep / live_compact(Soft): UNCHANGED
    (Soft itself does not relocate per AC2)

Contract (one row per AC):
  AC1  lifetime_consistency_proof.hh defines the consult helper
       (consult_last_lcp_for_densify_entry + DensifyEntryLCPPoll struct)
  AC2  lifetime_consistency_proof.hh defines
       g_densify_entry_lcp_blocked_total counter + reset hook
  AC3  evaluator_mutation_boundary.cpp Phase-5 main entry consults LCP
       (production_defaults_active || Full guard + helper call + counter
       bump + pin_contract_held forced false on block)
  AC4  evaluator_mutation_boundary.cpp optional one-shot Moving densify
       (recover_moving_sticky_densify_off) ALSO consults LCP (mirror of
       AC3 — same surface as Phase-5 entry)
  AC5  evaluator_gc.cpp compact_sweep / live_compact(Soft) is UNCHANGED:
       no LCP consult, no Issue #3185 cite (Soft itself does not
       relocate per AC2 — zero-cost contract)
  AC6  tests/core/test_moving_densify_fail_closed.cpp extended with
       ac3185_1-5 source-cite ACs (extends existing fail-closed test
       per #81967)
  AC7  linter wired in build.py after #3184, no docs/design/, no
       tests/issues/test_issue_3185.cpp (#81934)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_densify_entry_lcp_consult_3185.py            # report
  python3 scripts/coverage/checks/check_densify_entry_lcp_consult_3185.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_densify_entry_lcp_consult_3185.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

LCP = ROOT / "src" / "core" / "lifetime_consistency_proof.hh"
MUTATION_BOUNDARY = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
EVALUATOR_GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
TEST_FAIL_CLOSED = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3185.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _count(haystack: str, needle: str) -> int:
    if not needle:
        return 0
    n = 0
    pos = 0
    while True:
        p = haystack.find(needle, pos)
        if p == -1:
            break
        n += 1
        pos = p + 1
    return n


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3185 densify-entry LCP consult contract")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    lcp = _read(LCP)
    mut = _read(MUTATION_BOUNDARY)
    gc = _read(EVALUATOR_GC)
    test = _read(TEST_FAIL_CLOSED)
    build = _read(BUILD_PY)

    # ── AC1: lifetime_consistency_proof.hh defines the consult helper ──
    ac1_poll_struct = "struct DensifyEntryLCPPoll" in lcp
    ac1_poll_fields = (
        "bool present = false" in lcp
        and "bool would_allow_commit = true" in lcp
        and "std::uint32_t force_reason_code = 0" in lcp
    )
    ac1_helper = "[[nodiscard]] inline DensifyEntryLCPPoll consult_last_lcp_for_densify_entry()" in lcp
    ac1_helper_body_present = "last_lifetime_consistency_proof_present()" in lcp
    ac1_helper_body_would = "last_lifetime_consistency_would_allow()" in lcp
    ac1_helper_body_reason = "last_lifetime_consistency_force_reason()" in lcp
    ac1_cite = "Issue #3185" in lcp
    ac1_ok = (
        ac1_poll_struct
        and ac1_poll_fields
        and ac1_helper
        and ac1_helper_body_present
        and ac1_helper_body_would
        and ac1_helper_body_reason
        and ac1_cite
    )
    if not ac1_poll_struct:
        fails.append("AC1: DensifyEntryLCPPoll struct missing in lifetime_consistency_proof.hh")
    if not ac1_poll_fields:
        fails.append("AC1: DensifyEntryLCPPoll must expose present + would_allow_commit + force_reason_code")
    if not ac1_helper:
        fails.append(
            "AC1: consult_last_lcp_for_densify_entry() helper definition missing in lifetime_consistency_proof.hh"
        )
    if not (ac1_helper_body_present and ac1_helper_body_would and ac1_helper_body_reason):
        fails.append(
            "AC1: consult helper must populate present / would_allow_commit / force_reason_code from existing atomics"
        )
    if not ac1_cite:
        fails.append("AC1: cite Issue #3185 missing in lifetime_consistency_proof.hh")
    rows.append(
        {
            "ac": "AC1_consult_helper",
            "ok": ac1_ok,
            "poll_struct": ac1_poll_struct,
            "poll_fields": ac1_poll_fields,
            "helper_defined": ac1_helper,
            "helper_body_complete": ac1_helper_body_present and ac1_helper_body_would and ac1_helper_body_reason,
            "cites_3185": ac1_cite,
        }
    )

    # ── AC2: g_densify_entry_lcp_blocked_total counter + reset hook ──
    ac2_counter = "inline std::atomic<std::uint64_t>& g_densify_entry_lcp_blocked_total()" in lcp
    ac2_counter_init = "static std::atomic<std::uint64_t> v{0}" in lcp
    ac2_reset = "inline void reset_densify_entry_lcp_blocked_for_test()" in lcp
    ac2_reset_store = "g_densify_entry_lcp_blocked_total().store(0, std::memory_order_relaxed)" in lcp
    ac2_ok = ac2_counter and ac2_counter_init and ac2_reset and ac2_reset_store
    if not ac2_counter:
        fails.append("AC2: g_densify_entry_lcp_blocked_total() accessor missing")
    if not ac2_counter_init:
        fails.append("AC2: g_densify_entry_lcp_blocked_total() static atomic init missing")
    if not ac2_reset:
        fails.append("AC2: reset_densify_entry_lcp_blocked_for_test() test hook missing")
    if not ac2_reset_store:
        fails.append("AC2: reset hook must store(0) on the same atomic")
    rows.append(
        {
            "ac": "AC2_blocked_counter",
            "ok": ac2_ok,
            "counter_accessor": ac2_counter,
            "counter_init": ac2_counter_init,
            "reset_hook": ac2_reset,
            "reset_stores_zero": ac2_reset_store,
        }
    )

    # ── AC3: Phase-5 main entry consults LCP ──
    # Anchor on the unique Phase-5 cite comment (only at the Phase-5 entry).
    # Take a 3000-char forward window covering the consult + bump +
    # pin_contract_held forced-false block (which lives at the trailing end
    # of the entry, ~33 lines after the consult cite).
    phase5_anchor = "Issue #3185 AC1: consult last LifetimeConsistencyProof before"
    phase5_pos = mut.find(phase5_anchor)
    if phase5_pos != -1:
        phase5_window_end = min(len(mut), phase5_pos + 3000)
        phase5_window = mut[phase5_pos:phase5_window_end]
    else:
        phase5_window = ""
    ac3_guard_phase5 = "typed_audit::production_defaults_active() ||" in phase5_window
    ac3_helper_phase5 = "consult_last_lcp_for_densify_entry()" in phase5_window
    ac3_poll_check_phase5 = "poll.present && !poll.would_allow_commit" in phase5_window
    # Counter bump is multi-line in source:
    #   g_densify_entry_lcp_blocked_total()
    #       .fetch_add(1, std::memory_order_relaxed);
    # Match on the function-call tail instead of the contiguous literal.
    ac3_counter_bump_phase5 = (
        "g_densify_entry_lcp_blocked_total()" in phase5_window
        and ".fetch_add(1, std::memory_order_relaxed)" in phase5_window
    )
    ac3_pin_force_phase5 = "compact_r.pin_contract_held && !densify_entry_lcp_blocked" in phase5_window
    ac3_ok = (
        ac3_guard_phase5
        and ac3_helper_phase5
        and ac3_poll_check_phase5
        and ac3_counter_bump_phase5
        and ac3_pin_force_phase5
    )
    if phase5_pos == -1:
        fails.append(f"AC3: Phase-5 cite '{phase5_anchor}' missing in evaluator_mutation_boundary.cpp")
    if not ac3_guard_phase5:
        fails.append("AC3: Phase-5 entry missing production_defaults_active || Full guard")
    if not ac3_helper_phase5:
        fails.append("AC3: Phase-5 entry missing consult_last_lcp_for_densify_entry() call")
    if not ac3_poll_check_phase5:
        fails.append("AC3: Phase-5 entry missing poll.present && !poll.would_allow_commit check")
    if not ac3_counter_bump_phase5:
        fails.append("AC3: Phase-5 entry missing g_densify_entry_lcp_blocked_total().fetch_add(1, ...) bump")
    if not ac3_pin_force_phase5:
        fails.append(
            "AC3: Phase-5 entry missing pin_contract_held = compact_r.pin_contract_held && !densify_entry_lcp_blocked"
        )
    rows.append(
        {
            "ac": "AC3_phase5_entry_consult",
            "ok": ac3_ok,
            "cite_anchor_found": phase5_pos != -1,
            "guard": ac3_guard_phase5,
            "helper_call": ac3_helper_phase5,
            "poll_check": ac3_poll_check_phase5,
            "counter_bump": ac3_counter_bump_phase5,
            "pin_force": ac3_pin_force_phase5,
        }
    )

    # ── AC4: Optional one-shot Moving densify (recover_moving_sticky_densify_off) ──
    # Anchor on the unique one-shot cite comment (only inside the recover
    # function body). Take a 3000-char forward window covering the consult +
    # bump + pin_contract_held forced-false block (which lives at the
    # trailing end of the if-block, ~22 lines after the consult cite).
    oneshot_anchor = "Issue #3185 AC1: same surface as Phase-5 densify entry"
    oneshot_pos = mut.find(oneshot_anchor)
    if oneshot_pos != -1:
        oneshot_window_end = min(len(mut), oneshot_pos + 3000)
        oneshot_window = mut[oneshot_pos:oneshot_window_end]
    else:
        oneshot_window = ""
    # Walk back to confirm the function name on the same block (anchor comment
    # should be inside recover_moving_sticky_densify_off). Look up to 3000 chars
    # before the anchor for the function definition.
    oneshot_pre_window = mut[max(0, oneshot_pos - 3000) : oneshot_pos] if oneshot_pos != -1 else ""
    ac4_in_recover = "Evaluator::recover_moving_sticky_densify_off" in oneshot_pre_window
    ac4_guard = "typed_audit::production_defaults_active() ||" in oneshot_window
    ac4_helper = "consult_last_lcp_for_densify_entry()" in oneshot_window
    ac4_poll_check = "poll.present && !poll.would_allow_commit" in oneshot_window
    ac4_counter_bump = (
        "g_densify_entry_lcp_blocked_total()" in oneshot_window
        and ".fetch_add(1, std::memory_order_relaxed)" in oneshot_window
    )
    ac4_pin_force = "compact_r.pin_contract_held && !densify_entry_lcp_blocked" in oneshot_window
    ac4_ok = ac4_in_recover and ac4_guard and ac4_helper and ac4_poll_check and ac4_counter_bump and ac4_pin_force
    if oneshot_pos == -1:
        fails.append(f"AC4: optional one-shot cite '{oneshot_anchor}' missing in evaluator_mutation_boundary.cpp")
    if not ac4_in_recover:
        fails.append("AC4: optional one-shot consult site not inside recover_moving_sticky_densify_off")
    if not ac4_guard:
        fails.append("AC4: optional one-shot missing production_defaults_active || Full guard")
    if not ac4_helper:
        fails.append("AC4: optional one-shot missing consult_last_lcp_for_densify_entry() call")
    if not ac4_poll_check:
        fails.append("AC4: optional one-shot missing poll.present && !poll.would_allow_commit check")
    if not ac4_counter_bump:
        fails.append("AC4: optional one-shot missing g_densify_entry_lcp_blocked_total().fetch_add(1, ...) bump")
    if not ac4_pin_force:
        fails.append(
            "AC4: optional one-shot missing pin_contract_held = compact_r.pin_contract_held && !densify_entry_lcp_blocked"
        )
    rows.append(
        {
            "ac": "AC4_one_shot_recover_consult",
            "ok": ac4_ok,
            "cite_anchor_found": oneshot_pos != -1,
            "in_recover": ac4_in_recover,
            "guard": ac4_guard,
            "helper_call": ac4_helper,
            "poll_check": ac4_poll_check,
            "counter_bump": ac4_counter_bump,
            "pin_force": ac4_pin_force,
        }
    )

    # ── AC5: soft live_compact unchanged — no LCP consult in evaluator_gc.cpp ──
    ac5_no_helper_call = "consult_last_lcp_for_densify_entry" not in gc
    ac5_no_counter_ref = "g_densify_entry_lcp_blocked_total" not in gc
    ac5_no_3185_cite = "Issue #3185" not in gc
    ac5_soft_compact_intact = "LiveCompactMode::Soft" in gc
    ac5_ok = ac5_no_helper_call and ac5_no_counter_ref and ac5_no_3185_cite and ac5_soft_compact_intact
    if not ac5_no_helper_call:
        fails.append(
            "AC5: evaluator_gc.cpp must NOT call consult_last_lcp_for_densify_entry (Soft is zero-cost per AC2)"
        )
    if not ac5_no_counter_ref:
        fails.append("AC5: evaluator_gc.cpp must NOT reference g_densify_entry_lcp_blocked_total (Soft is zero-cost)")
    if not ac5_no_3185_cite:
        fails.append("AC5: evaluator_gc.cpp must NOT carry 'Issue #3185' cite (Soft live_compact path is unchanged)")
    if not ac5_soft_compact_intact:
        fails.append("AC5: live_compact(Soft) call site must remain in evaluator_gc.cpp (regression guard)")
    rows.append(
        {
            "ac": "AC5_soft_live_compact_unchanged",
            "ok": ac5_ok,
            "no_helper_call": ac5_no_helper_call,
            "no_counter_ref": ac5_no_counter_ref,
            "no_3185_cite": ac5_no_3185_cite,
            "soft_compact_intact": ac5_soft_compact_intact,
        }
    )

    # ── AC6: test_moving_densify_fail_closed.cpp extended with ac3185_1-5 ──
    ac6_test_exists = TEST_FAIL_CLOSED.is_file()
    ac6_marker = "Issue #3185: densify-entry LCP consult (steal×GC residual)" in test
    ac6_ac1 = "ac3185_1:" in test
    ac6_ac2 = "ac3185_2:" in test
    ac6_ac3 = "ac3185_3:" in test
    ac6_ac4 = "ac3185_4:" in test
    ac6_ac5 = "ac3185_5:" in test
    ac6_consult_assert = "consult_last_lcp_for_densify_entry" in test
    ac6_gc_assert = "evaluator_gc.cpp compact_sweep / live_compact(Soft) does NOT consult LCP" in test
    ac6_ok = (
        ac6_test_exists
        and ac6_marker
        and ac6_ac1
        and ac6_ac2
        and ac6_ac3
        and ac6_ac4
        and ac6_ac5
        and ac6_consult_assert
        and ac6_gc_assert
    )
    if not ac6_test_exists:
        fails.append(f"AC6: {TEST_FAIL_CLOSED} missing — extend existing fail-closed test per #81967")
    if not ac6_marker:
        fails.append("AC6: tests/core/test_moving_densify_fail_closed.cpp must carry the Issue #3185 marker")
    for n in (1, 2, 3, 4, 5):
        if not {"ac6_ac1": ac6_ac1, "ac6_ac2": ac6_ac2, "ac6_ac3": ac6_ac3, "ac6_ac4": ac6_ac4, "ac6_ac5": ac6_ac5}[
            f"ac6_ac{n}"
        ]:
            fails.append(f"AC6: ac3185_{n} source-cite AC missing in test_moving_densify_fail_closed.cpp")
    if not ac6_consult_assert:
        fails.append("AC6: ac3185_1 consult_last_lcp_for_densify_entry assertion missing")
    if not ac6_gc_assert:
        fails.append("AC6: ac3185_5 evaluator_gc.cpp soft-compact-unchanged assertion missing")
    rows.append(
        {
            "ac": "AC6_test_extension",
            "ok": ac6_ok,
            "test_exists": ac6_test_exists,
            "marker": ac6_marker,
            "ac3185_1": ac6_ac1,
            "ac3185_2": ac6_ac2,
            "ac3185_3": ac6_ac3,
            "ac3185_4": ac6_ac4,
            "ac3185_5": ac6_ac5,
            "consult_assert": ac6_consult_assert,
            "gc_unchanged_assert": ac6_gc_assert,
        }
    )

    # ── AC7: linter wired in build.py after #3184, no docs/design/, no tests/issues/ ──
    ac7_wired = "check_densify_entry_lcp_consult_3185" in build
    # Position must come after #3184 wire-in (string check; build.py is appended
    # in issue order). Confirm the build.py wire-in calls this linter.
    ac7_wired_after_3184 = (
        (build.find("check_densify_entry_lcp_consult_3185") > build.find("check_abort_restore_force_dirty_3184"))
        if ac7_wired
        else False
    )
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3185-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3185.cpp present (forbidden per #81934)")
    ac7_ok = ac7_wired and ac7_wired_after_3184 and no_design and no_issues_test
    if not ac7_wired:
        fails.append("AC7: linter not wired in build.py")
    if not ac7_wired_after_3184:
        fails.append("AC7: linter must be wired in build.py AFTER #3184 linter (issue-number ordering)")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "linter_wired": ac7_wired,
            "linter_wired_after_3184": ac7_wired_after_3184,
            "no_design_docs": no_design,
            "no_issue_test": no_issues_test,
        }
    )

    # ── Report ──
    if args.json:
        out = {"ok": len(fails) == 0, "rows": rows, "fails": fails}
        print(json.dumps(out, indent=2))
        return 0 if (len(fails) == 0 or not args.strict) else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    for r in rows:
        print(f"OK  {r['ac']}")
    print(
        "\nOK: Issue #3185 densify-entry LCP consult (steal×GC residual) — Phase-5 + optional one-shot consult, Soft zero-cost, evaluator_gc.cpp unchanged"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

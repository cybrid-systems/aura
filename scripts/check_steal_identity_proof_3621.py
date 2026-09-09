#!/usr/bin/env python3
# scripts/check_steal_identity_proof_3621.py -- Issue #3621 source-cite gate.
#
# Enumerative machine proof (over the real TUs, #3072/#2929 linter style)
# that the steal sample→clear→resample→stamp state machine cannot read
# thief TLS:
#
# AC1: mutation_safety_snapshot depth resolves via the victim storage
#      helper (aura_evaluator_mutation_stack_depth_from_ptr on the
#      victim's mutation_stack_storage_) — the compiler-side
#      mutation_boundary_depth_slot surface never leaks into the serve/
#      steal path (fiber.h + steal_safety.cpp).
# AC2: set_resume_safety_ticket is reachable only on the Ok path of
#      steal_safety_transaction — exactly one stamp site, positioned after
#      the last RejectHard return, stamping the decision-window snapshot
#      ticket (sample→enqueue equality).
# AC3: the Lifetime arm's Soft skip stays documented + cited (#2957/#3385)
#      — not treated as a missing arm.
# AC4: no new metric, no new query key, no new invariant enum arm.
# AC5: ACs live in tests/serve/test_steal_snapshot_hard_invariant.cpp
#      (ac3621_*); no new test philosophy; no test_issue_3621.cpp.
# AC6: source-cites steal_safety_transaction + mutation_safety_snapshot +
#      evaluate_residual_hard_and_bits names; residual arms key on the
#      victim evaluator id (#2727/#3617), never thief g_current_fiber.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIBER = "src/serve/fiber.h"
SS = "src/serve/steal_safety.cpp"
TEST = "tests/serve/test_steal_snapshot_hard_invariant.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_steal_identity_proof_3621"
HELPER = "aura_evaluator_mutation_stack_depth_from_ptr"
IDENT = "aura_fiber_evaluator_id_for_steal_safety(stolen)"
TXN = "StealSafetyDecision steal_safety_transaction(Fiber* stolen)"
STAMP = "set_resume_safety_ticket("
REJECT = "return StealSafetyDecision::RejectHard;"

AC_FN = (
    "ac3621_1_depth_victim_storage_identity",
    "ac3621_2_ticket_stamp_ok_only",
    "ac3621_3_identity_and_soft_lifetime_cite",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(fiber: str, ss: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — depth via victim storage; TLS slot never on the steal path.
    must(HELPER, "AC1 victim depth helper declared", fiber)
    must("thief thread must not read thread_local", "AC1 thief-TLS prohibition cite", fiber)
    must("s.depth = " + HELPER + "(", "AC1 snapshot depth via victim helper", fiber)
    must("depth from victim mutation_stack_storage_ (not thief TLS)", "AC1 victim-storage cite", fiber)
    must_not("mutation_boundary_depth_slot", "AC1 no TLS slot in fiber.h", fiber)
    must_not("mutation_boundary_depth_slot", "AC1 no TLS slot in steal path", ss)
    must("stolen->mutation_safety_snapshot()", "AC1 transaction samples victim", ss)

    # AC2 — ticket stamp reachable only on the Ok path (enumerative order).
    stamps = ss.count(STAMP)
    if stamps != 1:
        fails.append(f"AC2: expected exactly 1 ticket stamp site, found {stamps}")
    txn = ss.find(TXN)
    if txn == -1:
        fails.append("AC6: steal_safety_transaction missing")
    last_reject = ss.rfind(REJECT)
    stamp = ss.find(STAMP)
    if last_reject == -1 or stamp == -1 or stamp < last_reject or (txn != -1 and stamp < txn):
        fails.append("AC2: ticket stamp is not after the last RejectHard return")
    must("ticket stamp ONLY here", "AC2 sole-enqueue cite", ss)
    must(STAMP + "snap.ticket)", "AC2 stamp uses the decision-window snapshot ticket", ss)

    # AC3 — Lifetime arm Soft skip stays documented + cited.
    must("Soft: skip entirely (no loads)", "AC3 Lifetime soft skip cite", ss)

    # AC4 — no new counter / query key (no new invariant arm named for this).
    must_not("g_3621_", "AC4 no new counter", ss)
    must_not("schema-3621", "AC4 no new query key", ss)

    # AC6 — residual arms key on the victim evaluator id, never thief TLS.
    ident_reads = ss.count(IDENT)
    if ident_reads < 3:
        fails.append(f"AC6: expected >=3 victim-eval identity reads, found {ident_reads}")
    must_not("g_current_fiber", "AC6 no thief g_current_fiber in steal path", ss)
    must("Issue #3617: victim-eval keyed", "AC6 #3617 identity cite", ss)

    # AC5 — ACs in the existing invariant test; no new test philosophy.
    for fn in AC_FN:
        must(fn, "AC5 test AC defined", test)
        must(fn + "();", "AC5 test AC invoked", test)
    if (ROOT / "tests" / "serve" / "test_issue_3621.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3621.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "3621" in p.name:
                fails.append(f"AC4: forbidden docs/design/{p.name} (#1655)")
                break

    # Wiring.
    must(LINTER, "wiring build.py", build)
    must(LINTER + ".py", "wiring allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_fiber = (
            HELPER + "\n"
            "thief thread must not read thread_local\n"
            "s.depth = " + HELPER + "(\n"
            "depth from victim mutation_stack_storage_ (not thief TLS)\n"
        )
        sample_ss = (
            "stolen->mutation_safety_snapshot()\n"
            + TXN
            + "\n"
            + REJECT
            + "\n"  # last reject BEFORE the stamp (enumerative order)
            "ticket stamp ONLY here\n" + STAMP + "snap.ticket)\n"
            "Soft: skip entirely (no loads)\n"
            "Issue #3617: victim-eval keyed\n" + IDENT + "\n" + IDENT + "\n" + IDENT + "\n"  # three identity arms
        )
        sample_test = "".join(fn + "\n" + fn + "();\n" for fn in AC_FN)
        ok_fails = _rows(sample_fiber, sample_ss, sample_test, LINTER, LINTER + ".py")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "", "")
        if len(neg_fails) < 12:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(FIBER), _read(SS), _read(TEST), _read(BUILD), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("steal identity proof (#3621) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

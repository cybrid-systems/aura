#!/usr/bin/env python3
# scripts/check_mailbox_holder_send_lock_order_3613.py -- Issue #3613 source-cite gate.
#
# AC1: MultiFiberMailbox::push (multi_fiber_mailbox.h) runs the #2849
#      under-boundary note BEFORE on_acquire(Mailbox)/mu_ — a Guard-held
#      sender BPs without any lock acquisition (no Workspace→Mailbox
#      inversion; production canary/hard no longer aborts).
# AC2: broadcast_fanout runs the same note BEFORE mu_ (all-or-nothing,
#      note_self_backpressure from_fanout pairing kept).
# AC3: both old in-lock gates are replaced with the #3613 moved-cite.
# AC4: no new metrics field / query key — the helper + existing counters
#      are the only observables.
# AC5: ac3613 rows live in test_mailbox_hold_starvation_hard.cpp + the
#      lock-order canary suite; no docs/design/*3613-*, no
#      tests/**/test_issue_3613.cpp.
# AC6: build.py wires this linter.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MB = "src/serve/multi_fiber_mailbox.h"
TEST = "tests/serve/test_mailbox_hold_starvation_hard.cpp"
CANARY = "tests/compiler/test_lock_order_audit.cpp"
BUILD = "build.py"
QUERY = "src/compiler/evaluator_primitives_obs_eval.cpp"
OBS = "src/compiler/observability_metrics.h"

PUSH_CITE = "Issue #3613: BEFORE mu_"
FANOUT_CITE = "Issue #3613: same holder-send inversion fix as push()"
NOTE = "if (note_mailbox_deferred_under_boundary(&local_stats_))"
MOVED = "moved BEFORE mu_ (#3613"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(mb: str, test: str, canary: str, build: str, query: str, obs: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1: push — note BEFORE on_acquire(Mailbox).
    must(PUSH_CITE, "AC1 push cite", mb)
    p1 = mb.find(PUSH_CITE)
    if p1 < 0:
        fails.append("AC1: push window unreachable")
    else:
        win = mb[p1 : p1 + 1400]
        must(NOTE, "AC1 note call", win)
        must("on_acquire(::aura::compiler::lock_order::Level::Mailbox", "AC1 on_acquire", win)
        if (
            NOTE in win
            and "on_acquire(::aura::compiler::lock_order::Level::Mailbox" in win
            and win.find(NOTE) > win.find("on_acquire(::aura::compiler::lock_order::Level::Mailbox")
        ):
            fails.append("AC1: note must precede on_acquire(Mailbox)")

    # AC2: fanout — note BEFORE mu_.
    must(FANOUT_CITE, "AC2 fanout cite", mb)
    p2 = mb.find(FANOUT_CITE)
    if p2 < 0:
        fails.append("AC2: fanout window unreachable")
    else:
        win2 = mb[p2 : p2 + 900]
        must(NOTE, "AC2 note call", win2)
        must("std::lock_guard lock(mu_)", "AC2 mu_", win2)
        if (
            NOTE in win2
            and "std::lock_guard lock(mu_)" in win2
            and win2.find(NOTE) > win2.find("std::lock_guard lock(mu_)")
        ):
            fails.append("AC2: note must precede mu_")

    # AC3: both old in-lock gates replaced.
    must(MOVED, "AC3 old gates replaced", mb)

    # AC4: no new observables.
    must_not("3613", "AC4 no new metrics field", obs)
    must_not("schema-3613", "AC4 no new query key", query)

    # AC5: test rows present; no invent.
    must("ac3613_1_holder_send_bp_before_lock", "AC5 starvation AC1", test)
    must("ac3613_4_source_and_linter", "AC5 starvation source", test)
    must("ac3613_canary_holder_send_no_inversion", "AC5 canary row", canary)
    must("Issue #3613", "AC5 test cite", test)
    for rel in (
        "tests/issues/test_issue_3613.cpp",
        "tests/compiler/test_issue_3613.cpp",
        "tests/serve/test_issue_3613.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: forbidden {rel} exists (#81967)")
    if any(p.name.find("3613-") >= 0 for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC5: forbidden docs/design/*3613-* exists (#1655)")

    # AC6: build.py wiring.
    must("check_mailbox_holder_send_lock_order_3613", "AC6 build.py wiring", build)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_mb = (
            PUSH_CITE
            + "\n"
            + NOTE
            + "\non_acquire(::aura::compiler::lock_order::Level::Mailbox\n"
            + FANOUT_CITE
            + "\n"
            + NOTE
            + "\nstd::lock_guard lock(mu_)\n"
            + MOVED
            + "\n"
        )
        ok_fails = _rows(
            sample_mb,
            "ac3613_1_holder_send_bp_before_lock ac3613_4_source_and_linter Issue #3613",
            "ac3613_canary_holder_send_no_inversion",
            "check_mailbox_holder_send_lock_order_3613",
            "clean",
            "clean",
        )
        neg_fails = _rows("", "", "", "", "schema-3613", "3613")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        if len(neg_fails) < 10:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(MB), _read(TEST), _read(CANARY), _read(BUILD), _read(QUERY), _read(OBS))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("mailbox holder send lock order (#3613) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

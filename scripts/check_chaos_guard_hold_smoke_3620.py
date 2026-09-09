#!/usr/bin/env python3
# scripts/check_chaos_guard_hold_smoke_3620.py -- Issue #3620 source-cite gate.
#
# AC1: One fixed-seed case (ac3620_1_guard_held_mailbox_no_edge_hold_windows)
#      lives in the existing chaos binary and runs on the DEFAULT invocation
#      (no FULL=1 / gate env required).
# AC2: Hard-fail doors — push Ok while the shared Evaluator boundary is live,
#      steal Ok against the held fiber (snapshot mismatch delta), torn
#      query-stable export (query_stable_hard_reject_torn) all CHECK-fail.
# AC3: Soft / sandbox=off path of the same binary remains observe-only for
#      force-unlock (existing reject_enabled production split in fiber.cpp).
# AC4: No new counter, no new query key — reuse steal RejectHard bits +
#      mailbox BP + restamp torn. No docs/design/*3620-*, no
#      test_issue_3620.cpp.
# AC5: Cited in scripts/coverage/checks/check_chaos_soak_2679.py / existing
#      AC comments only.
# AC6: Case body source-cites both companion fixes at file:function:
#      #3613 -> src/serve/multi_fiber_mailbox.h
#               note_mailbox_deferred_under_boundary;
#      #3619 -> src/serve/fiber.cpp aura_mutation_hold_no_edge_still_held.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CHAOS = "tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp"
FCPP = "src/serve/fiber.cpp"
MBH = "src/serve/multi_fiber_mailbox.h"
SH = "src/serve/steal_safety.h"
SOAK = "scripts/coverage/checks/check_chaos_soak_2679.py"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

CASE = "ac3620_1_guard_held_mailbox_no_edge_hold_windows"
CITE = "ac3620_2_windows_source_cite"
LINTER = "check_chaos_guard_hold_smoke_3620"
NOTE3613 = "note_mailbox_deferred_under_boundary"
PROBE3619 = "aura_mutation_hold_no_edge_still_held"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(chaos: str, fcpp: str, mbh: str, sh: str, soak: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1: default-invocation fixed-seed case in the existing chaos binary.
    must(CASE, "AC1 case defined", chaos)
    must(CASE + "();", "AC1 case invoked from main flow", chaos)
    must(CITE, "AC1 source-cite AC defined", chaos)
    must("set_production_multi_worker_latched_for_test", "AC1 production latch seam", chaos)
    must("std::thread holder", "AC1 raw-thread holder (un-nudgeable #3619 window)", chaos)
    must("Fiber::yield(YieldReason::Explicit)", "AC1 cooperative peer fibers", chaos)
    must("Scheduler sched(2)", "AC1 N small (2 workers)", chaos)
    case_pos = chaos.find("static void " + CASE)
    if case_pos == -1:
        fails.append("AC1: case definition missing")
        case_win = ""
    else:
        case_win = chaos[case_pos : case_pos + 6000]
        must_not("AURA_CHAOS_FULL", "AC1 default invocation (no FULL gate)", case_win)

    # AC2: hard-fail doors on the three failure modes.
    must("push Ok while shared Evaluator boundary live", "AC2 push-Ok hard-fail", chaos)
    must("mutation_steal_snapshot_mismatch_total() == mismatch0", "AC2 steal-Ok hard-fail", chaos)
    must("query_stable_hard_reject_torn", "AC2 torn export hard-fail", chaos)
    must("aura_evaluator_mutation_boundary_depth() == 0", "AC2 depth clear post-poll", chaos)
    must("aura_hold_budget_poll_busy_path", "AC2 busy-path poll drives #3619", chaos)
    must(PROBE3619, "AC2 strong probe consulted", chaos)

    # AC3: Soft / sandbox=off remains observe-only for force-unlock.
    must("if (!mutation_hold_budget_reject_enabled())", "AC3 soft split gate", fcpp)
    must("Soft / sandbox=off: metric-only", "AC3 soft observe-only cite", fcpp)

    # AC4: no new counter / query key / docs / invented test file. Scan the
    # production surfaces (the case's own CHECK strings mention the needles,
    # so scanning the chaos file would self-match).
    must_not("g_3620_", "AC4 no new counter", fcpp)
    must_not("schema-3620", "AC4 no new query key", mbh)
    if (ROOT / "tests" / "serve" / "test_issue_3620.cpp").is_file():
        fails.append("AC4: forbidden tests/serve/test_issue_3620.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "3620" in p.name:
                fails.append(f"AC4: forbidden docs/design/{p.name} (#1655)")
                break

    # AC5: cited in the existing soak linter / AC comments only.
    must("3620", "AC5 soak linter cite", soak)

    # AC6: case body + production anchors at file:function.
    must(NOTE3613, "AC6 #3613 helper cite in case", chaos)
    must(PROBE3619, "AC6 #3619 probe cite in case", chaos)
    must("Issue #3613", "AC6 #3613 production cite", mbh)
    must(PROBE3619, "AC6 #3619 probe def", fcpp)
    must("g_hold_budget_no_edge_force_total", "AC6 #3619 no-edge counter", fcpp)
    must(PROBE3619, "AC6 #3619 residual-zero reader consult", sh)

    # Wiring: build.py + allowlist.
    must(LINTER, "wiring build.py", build)
    must(LINTER + ".py", "wiring allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_chaos = (
            CASE + "\nstatic void " + CASE + '() { std::println("#3620"); }\n' + CASE + "();\n" + CITE + "();\n"
            "set_production_multi_worker_latched_for_test\n"
            "spawn_with_affinity\n"
            "Scheduler sched(2)\n"
            "push Ok while shared Evaluator boundary live\n"
            "mutation_steal_snapshot_mismatch_total() == mismatch0\n"
            "query_stable_hard_reject_torn\n"
            "aura_evaluator_mutation_boundary_depth() == 0\n"
            "aura_hold_budget_poll_busy_path\n" + PROBE3619 + "\n" + NOTE3613 + "\n"
            "#3613 #3619\n"
        )
        sample_fcpp = (
            "if (!mutation_hold_budget_reject_enabled())\n"
            "Soft / sandbox=off: metric-only\n" + PROBE3619 + "\n"
            "g_hold_budget_no_edge_force_total\n"
        )
        ok_fails = _rows(
            sample_chaos,
            sample_fcpp,
            "Issue #3613 " + NOTE3613,
            PROBE3619,
            "3620",
            LINTER,
            LINTER + ".py",
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

    fails = _rows(_read(CHAOS), _read(FCPP), _read(MBH), _read(SH), _read(SOAK), _read(BUILD), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("chaos guard-hold smoke (#3620) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

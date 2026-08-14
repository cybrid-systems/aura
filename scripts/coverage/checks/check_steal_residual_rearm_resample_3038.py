#!/usr/bin/env python3
"""Issue #3038: re-sample residual hard-AND after clear under per-Fiber window.

#2901 / #2954 close the re-arm window with a counter and per-Fiber CAS.
Clear still ran *outside* the decision window — a theoretical re-arm
between that clear and the residual sample could stamp a ticket.

Contract:
  AC1 injected re-arm after clear → RejectHard, no ticket, rearm_race bumps
  AC2 quiet path (no residual) zero extra work beyond current
  AC3 same StealInvariant table, no new arms
  AC4 existing residual_rearm_race_total remains the SSOT
  AC5 extend residual_rearm / steal_complete tests + chaos soak forced hook

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

    hdr = _read("src/serve/steal_safety.h")
    cpp = _read("src/serve/steal_safety.cpp")
    t = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    chaos = _read("tests/serve/test_chaos_steal_mutation_gc.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    build = _read("build.py")

    must("Issue #3038", "AC1", cpp)
    must("kStealSafetyResidualRearmResampleIssue = 3038", "AC1", hdr)
    must("StealDecisionGuard", "AC1 window", cpp)
    must("aura_evaluator_on_steal_complete(stolen)", "AC1 clear", cpp)
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC1 hook", cpp)
    must("evaluate_residual_hard_and_bits", "AC1 re-sample", cpp)
    must("g_steal_safety_residual_rearm_race_total", "AC1 SSOT", cpp)
    must("set_resume_safety_ticket(snap.ticket)", "AC1 stamp after ok", cpp)
    must("ac3038_1_inject_rearm_after_clear_no_ticket", "AC1 test", t)
    if "std::mutex g_steal_safety_decision_mu" in cpp or "lock_guard" in cpp:
        fails.append("AC1: process-wide decision mutex still present")

    must("ac3038_2_quiet_path_no_extra", "AC2", t)
    must("g_steal_safety_between_clear_and_hard_and_hook != nullptr", "AC2 hook gate", cpp)
    must("/*bump_counters=*/false", "AC2 quiet re-sample", cpp)

    must("ac3038_3_same_invariant_table", "AC3", t)
    must("StealInvariant::BoundarySafe", "AC3", cpp)
    must("StealInvariant::LayoutStampMatch", "AC3", cpp)
    must("StealInvariant::TicketFresh", "AC3", cpp)
    must("StealInvariant::GcDeferClear", "AC3", cpp)
    must("Count = 7", "AC3 no new arm", hdr)

    must("ac3038_4_ssot_counter", "AC4", t)
    must("kStealSafetyResidualRearmRaceIssue = 2901", "AC4", hdr)
    must("same counter is SSOT", "AC4", hdr)
    must("steal-safety-residual-rearm-race-total", "AC4 query SSOT", q)
    must("schema-2901", "AC4 lineage", q)
    must("schema-3038", "AC4 wired", q)

    must("ac3038_5_source_chaos_linter", "AC5", t)
    must("ac3038_forced_hook_rearm", "AC5 chaos", chaos)
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC5 chaos hook", chaos)
    must("check_steal_residual_rearm_resample_3038", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_3038.cpp").is_file():
        fails.append("AC5: test_issue_3038.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3038-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3038 steal residual re-arm re-sample — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

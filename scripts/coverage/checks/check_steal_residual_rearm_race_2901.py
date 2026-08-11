#!/usr/bin/env python3
"""Issue #2901: residual re-arm race window inside steal_safety_transaction.

Contract:
  AC1 inject residual re-arm between clear and stamp → RejectHard; no ticket stamp
  AC2 happy path single on_steal_complete + hard-AND; quiet rearm_race
  AC3 existing residual_* + transaction counters additive
  AC4 RejectHard never enqueues (worker Ok gate)
  AC5 source-cite + extend test_steal_complete_restamp_txn; no docs/design/
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
    wc = _read("src/serve/worker.cpp")
    t = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    must("2901", "AC1", hdr)
    must("g_steal_safety_residual_rearm_race_total", "AC1", hdr)
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC1", hdr)
    must("g_steal_safety_decision_mu", "AC1", cpp)
    must("evaluate_residual_hard_and", "AC1", cpp)
    must("residual_rearm_race", "AC1", cpp)

    must("aura_evaluator_on_steal_complete", "AC2", cpp)
    must("bump_counters", "AC2", cpp)

    must("g_steal_safety_transaction_ok_total", "AC3", hdr)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC3", hdr)
    must("g_steal_safety_residual_ticket_mismatch_total", "AC3", hdr)

    must("StealSafetyDecision::Ok", "AC4", wc)
    must("steal_safety_transaction", "AC4", wc)

    must("ac2901_1_inject_rearm_rejects_no_ticket", "AC5", t)
    must("ac2901_2_happy_path_single_complete", "AC5", t)
    must("ac2901_3_counters_additive", "AC5", t)
    must("ac2901_4_chaos_contract_source", "AC5", t)
    must("ac2901_5_source_cite", "AC5", t)
    must("check_steal_residual_rearm_race_2901", "AC5", build)

    # No post-transaction residual path that can stamp after reject:
    # stamp must only appear once after residual_ok path.
    if "set_resume_safety_ticket" in cpp and cpp.count("set_resume_safety_ticket(snap.ticket)") != 1:
        fails.append("AC4: expected single set_resume_safety_ticket(snap.ticket) site")

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2901-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "serve" / "test_issue_2901.cpp").is_file():
        fails.append("tests/serve/test_issue_2901.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2901 steal residual re-arm race — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

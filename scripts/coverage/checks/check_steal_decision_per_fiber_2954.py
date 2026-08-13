#!/usr/bin/env python3
"""Issue #2954: per-Fiber steal decision protocol (replace global mu).

Preserve #2901 residual re-arm close; remove process-wide
g_steal_safety_decision_mu from Ok decision window.

Contract:
  AC1 no process-wide mutex; concurrent multi-victim ok
  AC2 #2901 re-arm still RejectHard + race counter
  AC3 Soft/production decision window always per-Fiber
  AC4 additive counters; #2699/#2721/#2901 preserved
  AC5 source-cite + tests + build.py
  AC6 no docs/design; no invent test file
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

    fh = _read("src/serve/fiber.h")
    cpp = _read("src/serve/steal_safety.cpp")
    hdr = _read("src/serve/steal_safety.h")
    mut = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    t = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2954", "AC1", fh)
    must("try_begin_steal_decision", "AC1", fh)
    must("steal_decision_busy_", "AC1", fh)
    must("StealDecisionGuard", "AC1", cpp)
    # Allow historical mention in comments ("Replaces process-wide …")
    # but forbid an actual mutex variable / lock_guard.
    if "std::mutex g_steal_safety_decision_mu" in cpp or "lock_guard" in cpp:
        fails.append("AC1: process-wide decision mutex / lock_guard still present")
    if "#include <mutex>" in cpp:
        fails.append("AC1: mutex include still in steal_safety.cpp")
    must("2954 AC1", "AC1", t)

    # AC2 #2901 contract
    must("residual_rearm_race", "AC2", cpp)
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC2", hdr)
    must("set_resume_safety_ticket(snap.ticket)", "AC2", cpp)
    must("2954 AC3", "AC2", t)  # rearm still RejectHard test

    # AC3 / AC4
    must("g_steal_decision_contention_total", "AC4", hdr)
    must("g_steal_decision_per_fiber_wired", "AC4", hdr)
    must("kStealDecisionPerFiberIssue = 2954", "AC4", hdr)
    must("g_steal_safety_residual_rearm_race_total", "AC4", hdr)
    must("g_steal_safety_transaction_ok_total", "AC4", hdr)
    must("steal-decision-contention-total", "AC4", mut)
    must("steal-decision-per-fiber-wired", "AC4", mut)
    must("schema-2954", "AC4", mut)
    must("schema-2901", "AC4", mut)

    # AC5 / AC6
    must("ac2954_1_no_process_wide_mutex", "AC5", t)
    must("ac2954_2_concurrent_multi_victim", "AC5", t)
    must("ac2954_3_rearm_still_reject_hard", "AC5", t)
    must("check_steal_decision_per_fiber_2954", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_2954.cpp").is_file():
        fails.append("AC6: test_issue_2954.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2954-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    # Single stamp site preserved (#2901 AC4)
    if cpp.count("set_resume_safety_ticket(snap.ticket)") != 1:
        fails.append("AC2: expected single set_resume_safety_ticket(snap.ticket) site")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2954 per-Fiber steal decision protocol")
    return 0


if __name__ == "__main__":
    sys.exit(main())

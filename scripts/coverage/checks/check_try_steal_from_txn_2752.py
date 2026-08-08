#!/usr/bin/env python3
"""Issue #2752: WorkerThread::try_steal_from only uses steal_safety_transaction.

Contract (one row per AC):
  AC1 src/serve/worker.cpp try_steal_from success path calls only
     steal_safety_transaction(stolen). On Ok → local_queue_.push; on
     RejectHard → return false (no enqueue). Pre-transaction inconsistency
     force-deopt path remains.
  AC2 No set_resume_safety_ticket in worker.cpp (ticket stamp only in
     steal_safety.cpp Ok branch). No call_steal_complete(stolen) on the
     success path (transaction owns aura_evaluator_on_steal_complete).
  AC3 Existing transaction + residual hard-AND counters remain additive.
  AC4 test_steal_complete_restamp_txn.cpp extended with ac2752_* blocks.
  AC5 This linter wired in build.py; no docs/design/2752-* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    worker = _read("src/serve/worker.cpp")
    ss = _read("src/serve/steal_safety.cpp")
    hdr = _read("src/serve/steal_safety.h")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1 — sole transaction entry on success path
    must("steal_safety_transaction(stolen)", "AC1", worker)
    must("StealSafetyDecision::Ok", "AC1", worker)
    must("try_steal_from_only_uses_steal_safety_transaction", "AC1 marker", worker)
    must('#include "serve/steal_safety.h"', "AC1 include", worker)
    # Order: transaction before Ok gate before push (best-effort string order)
    txn_pos = worker.find("steal_safety_transaction(stolen)")
    ok_pos = worker.find("StealSafetyDecision::Ok")
    push_pos = worker.find("local_queue_.push(stolen)")
    if not (0 <= txn_pos < ok_pos < push_pos):
        fails.append("AC1: expected steal_safety_transaction → Ok → local_queue_.push order")

    # AC2 — no direct ticket stamp / call_steal_complete on success path.
    # Comments may mention set_resume_safety_ticket; forbid call-shaped uses.
    must_not("set_resume_safety_ticket(", "AC2 worker call", worker)
    must_not("->set_resume_safety_ticket", "AC2 worker call", worker)
    must_not("call_steal_complete(stolen)", "AC2 worker call site", worker)
    must("stolen->set_resume_safety_ticket(snap.ticket)", "AC2 stamp in steal_safety.cpp", ss)

    # AC3 — counters additive
    must("g_steal_safety_transaction_calls_total", "AC3", hdr)
    must("g_steal_safety_transaction_reject_hard_total", "AC3", hdr)
    must("g_steal_safety_transaction_ok_total", "AC3", hdr)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC3", hdr)

    # AC4 — test extension
    must("ac2752_1_try_steal_from_only_transaction", "AC4", test)
    must("ac2752_2_no_ticket_stamp_in_worker", "AC4", test)
    must("ac2752_3_counters_additive", "AC4", test)
    must("ac2752_4_source_and_linter", "AC4", test)

    # AC5 — build.py wire + no docs/design
    must("check_try_steal_from_txn_2752", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2752-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2752 try_steal_from only steal_safety_transaction — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

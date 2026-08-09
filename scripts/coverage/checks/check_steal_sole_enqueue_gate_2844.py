#!/usr/bin/env python3
"""Issue #2844: steal_safety_transaction is the sole enqueue gate for stolen fibers.

Residual of #2699/#2721/#2752: production work-steal must treat
steal_safety_transaction(Fiber*) as the only decision that may
local_queue_.push a stolen fiber. Soft-continue after MutationSafetySnapshot
sample must never enqueue under production.

  AC1 try_steal_from: decision = steal_safety_transaction(stolen);
      Ok → local_queue_.push(stolen); RejectHard → return false (no push)
  AC2 No soft-enqueue after snapshot sample / inconsistency path
  AC3 Existing counters additive (transaction + residual hard-AND)
  AC4 Exactly one local_queue_.push(stolen); dominated by Ok gate
  AC5 Extend test_steal_complete_restamp_txn.cpp; linter wired; no docs/design/
  AC6 Soft metric-only only when !production (existing soft helpers)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    worker = _read("src/serve/worker.cpp")
    ss = _read("src/serve/steal_safety.cpp")
    hdr = _read("src/serve/steal_safety.h")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1 — sole transaction gate
    must("steal_safety_transaction(stolen)", "AC1", worker)
    must("StealSafetyDecision::Ok", "AC1", worker)
    must("local_queue_.push(stolen)", "AC1", worker)
    must("steal_safety_transaction_is_sole_enqueue_gate_for_stolen", "AC1 marker", worker)
    must("Issue #2844", "AC1", worker)
    # Order on code lines only (comments may mention Ok earlier).
    code_only_for_order = "\n".join(ln for ln in worker.splitlines() if not ln.lstrip().startswith("//"))
    txn_pos = code_only_for_order.find("steal_safety_transaction(stolen)")
    ok_pos = code_only_for_order.find("StealSafetyDecision::Ok")
    push_pos = code_only_for_order.find("local_queue_.push(stolen)")
    if not (0 <= txn_pos < ok_pos < push_pos):
        fails.append("AC1: expected steal_safety_transaction → Ok → local_queue_.push(stolen)")

    # AC2 — no soft-enqueue after snapshot inconsistency
    must("no soft-enqueue after snapshot sample", "AC2", worker)
    must("Never enqueue a stolen fiber after inconsistency sample", "AC2", worker)
    # Count non-comment local_queue_.push(stolen) — only one allowed.
    code_only = "\n".join(ln for ln in worker.splitlines() if not ln.lstrip().startswith("//"))
    stolen_pushes = len(re.findall(r"local_queue_\.push\(stolen\)", code_only))
    if stolen_pushes != 1:
        fails.append(f"AC2/AC4: expected exactly 1 code local_queue_.push(stolen), found {stolen_pushes}")

    # AC3 — counters additive
    must("g_steal_safety_transaction_calls_total", "AC3", hdr)
    must("g_steal_safety_transaction_reject_hard_total", "AC3", hdr)
    must("g_steal_safety_transaction_ok_total", "AC3", hdr)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC3", hdr)
    must("stolen->set_resume_safety_ticket(snap.ticket)", "AC3 ticket on Ok", ss)

    # AC4 — sole push(stolen) after Ok (order already checked)
    must("#2844 sole stolen-fiber enqueue", "AC4", worker)

    # AC5 — tests + linter + no invent/design
    must("ac2844_1_sole_enqueue_gate", "AC5", test)
    must("ac2844_2_no_soft_enqueue_after_sample", "AC5", test)
    must("ac2844_3_counters_preserved", "AC5", test)
    must("ac2844_4_source_and_linter", "AC5", test)
    must("check_steal_sole_enqueue_gate_2844", "AC5", build)
    # Prior surfaces preserved
    must("ac2752_1_try_steal_from_only_transaction", "AC5 #2752 preserved", test)
    must("ac2699_1_unified_entry_exists", "AC5 #2699 preserved", test)
    if (ROOT / "tests" / "serve" / "test_issue_2844.cpp").is_file():
        fails.append("AC5: test_issue_2844.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2844*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — Soft only when !production (existing helpers still present)
    must("is_steal_snapshot_soft_mode", "AC6", worker)
    must("steal_snapshot_soft_production_locked", "AC6", worker)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2844 steal_safety_transaction sole enqueue gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

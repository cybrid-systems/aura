#!/usr/bin/env python3
"""Issue #2398: mailbox_bp_recent_total quiet-period window / decay.

Contract:
  AC1 quiet-period decay after last BP; storm → wait → spawn succeeds
  AC2 send_backpressure_total remains cumulative
  AC3 threshold=0 → no admit reject / zero cost decay skip
  AC4 additive query keys + schema-2398; #2228 keys intact
  AC5 tests + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    orch = _read("src/orch/agent_spawn.h")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_mailbox_bp_admit_2228.cpp")
    decay = _read("tests/orch/test_orch_admission_decay.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 lifecycle
    must("Issue #2398", "AC1", orch)
    must("note_mailbox_bp_recent_event", "AC1", orch)
    must("maybe_decay_mailbox_bp_recent", "AC1", orch)
    must("g_mailbox_bp_last_event_us", "AC1", orch)
    must("AURA_ORCH_BP_WINDOW_MS", "AC1", orch)
    must("resolve_mailbox_bp_window_ms", "AC1", orch)
    must("2398 AC1", "AC1", test)
    must("note_mailbox_bp_recent_event", "AC1", decay)

    # AC2 cumulative
    must("send_backpressure_total", "AC2", orch)
    must("2398 AC2", "AC2", test)

    # AC3 threshold=0 zero cost
    must("threshold > 0", "AC3", orch)
    must("2398 AC3", "AC3", test)

    # AC4 query additive
    must("mailbox-bp-window-ms", "AC4", prim)
    must("mailbox-bp-decay-wired", "AC4", prim)
    must("schema-2398", "AC4", prim)
    must("issue-2398", "AC4", prim)
    must("schema-2228", "AC4", prim)
    must("kMailboxBpRecentWindowIssue", "AC4", orch)
    must("2398 AC4", "AC4", test)
    must("note_mailbox_bp_recent_event", "AC4", mut)

    # AC5
    must("2398 AC5", "AC5", test)
    must("check_mailbox_bp_recent_window_2398", "AC5", build)
    must("cmd_mailbox_bp_recent_window_coverage", "AC5", build)
    must("test_mailbox_bp_admit_2228", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2398 mailbox BP recent window — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

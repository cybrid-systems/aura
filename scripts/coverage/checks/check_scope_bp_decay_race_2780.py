#!/usr/bin/env python3
"""Issue #2780: maybe_decay vs note_mailbox_bp_recent race (#2633 residual).

Snapshot-under-lock then zero-outside-lock wiped concurrent fetch_add
→ silent BP event loss → under-admit on stormy scopes.

Contract (one row per AC):
  AC1 note_mailbox_bp_recent_event updates recent + last_event under map mutex
  AC2 maybe_decay zeros under map mutex (no snapshot.reserve + unlock pattern)
  AC3 skip gauges whose last_event_us is still inside the quiet window
  AC4 ac2780_* tests in test_mailbox_bp_admit; schema-2780 query keys
  AC5 this linter wired; no docs/design/2780-*; no test_issue_2780.cpp

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
            fails.append(f"{label}: forbidden residual {n!r}")

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_mailbox_bp_admit.cpp")
    build = _read("build.py")

    # AC1 — note under lock
    must("kMailboxBpScopeDecayRaceIssue", "AC1", spawn)
    must("2780", "AC1", spawn)
    must("note_mailbox_bp_recent_event", "AC1", spawn)
    # fetch_add and last_event_us still present on the scope path
    must("gauge->last_event_us.store", "AC1", spawn)
    must("gauge->recent.fetch_add", "AC1", spawn)
    must("g_scope_bp_map_mtx", "AC1", spawn)

    # AC2 — decay zeros under lock; no unlock-then-zero snapshot pattern
    must("maybe_decay_mailbox_bp_recent", "AC2", spawn)
    must("Issue #2780", "AC2", spawn)
    must_not("snapshot.reserve", "AC2", spawn)
    # Zero under the same mutex (lock held across the for-loop).
    must("std::lock_guard<std::mutex> lock(g_scope_bp_map_mtx)", "AC2", spawn)
    must("g->recent.store(0", "AC2", spawn)

    # AC3 — skip active by last_event_us
    must("still inside the quiet window", "AC3", spawn)
    must("last_event_us.load", "AC3", spawn)

    # AC4 — tests + query
    must("ac2780_concurrent_note_decay", "AC4", test)
    must("ac2780_skip_active", "AC4", test)
    must("ac2780_source_and_query", "AC4", test)
    must("schema-2780", "AC4", prim)
    must("scope-bp-decay-race-wired", "AC4", prim)

    # AC5
    must("check_scope_bp_decay_race_2780", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2780-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_2780.cpp").is_file():
        fails.append("AC5: test_issue_2780.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2780 scope BP decay race — note+decay under map mutex, skip active last_event_us, schema-2780")
    return 0


if __name__ == "__main__":
    sys.exit(main())

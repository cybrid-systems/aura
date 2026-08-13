#!/usr/bin/env python3
"""Issue #2945: Reclaimed join hash reservation-held + mailbox-held flags.

Refine #2885 / #2661 — Agents branch cleanup policy from join hash alone
without polling reserved_memory_bytes / mailbox residual C++ state.

Contract (one row per AC):
  AC1  reservation-held + mailbox-held on Reclaimed orch:agent-join hash
  AC2  Ok / Timeout / Cancelled paths do NOT add the new keys (zero-cost)
  AC3  #2661: no reservation release / mailbox detach on Reclaimed cleanup
  AC4  Interaction with #2924 / Done-path cleanup documented + tested
  AC5  Additive schema-2945; #2885 keys preserved
  AC6  Source-cite + tests in test_join_drain_reclaim; no invent/design

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

    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    spawn = _read("src/orch/agent_spawn.h")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # AC1
    must("reservation-held", "AC1", agent)
    must("mailbox-held", "AC1", agent)
    must("reserved_memory_bytes", "AC1", agent)
    must("Issue #2945", "AC1", agent)
    must("Issue #2945", "AC1", spawn)

    # AC2 — keys only under Reclaimed guard
    reclaimed_if = agent.find("if (jr.status == aura::serve::JoinStatus::Reclaimed)")
    res_key = agent.find('"reservation-held"')
    mb_key = agent.find('"mailbox-held"')
    if reclaimed_if < 0 or res_key < 0 or res_key < reclaimed_if:
        fails.append("AC2: reservation-held not guarded by Reclaimed status")
    if reclaimed_if < 0 or mb_key < 0 or mb_key < reclaimed_if:
        fails.append("AC2: mailbox-held not guarded by Reclaimed status")
    # still-running lineage preserved under same guard
    must("still-running", "AC2", agent)

    # AC3 — #2661 Reclaimed cleanup
    must("complete_agent_join_cleanup", "AC3", spawn)
    start = spawn.find("if (jr.status == serve::JoinStatus::Reclaimed)")
    if start < 0:
        fails.append("AC3: Reclaimed branch missing in complete_agent_join_cleanup")
    else:
        end = spawn.find("return;", start)
        if end < 0:
            end = start + 900
        block = spawn[start:end]
        if "release_orphan_roots" not in block:
            fails.append("AC3: Reclaimed must call release_orphan_roots")
        if "release_agent_memory_reservation" in block:
            fails.append("AC3: Reclaimed must NOT release_agent_memory_reservation")
        if "mailbox->detach" in block:
            fails.append("AC3: Reclaimed must NOT mailbox->detach")

    # AC4 — #2924 / wait interaction cited
    must("2945", "AC4", test)
    must("wait_reclaimed_body", "AC4", test)

    # AC5
    must("schema-2945", "AC5", agent)
    must("issue-2945", "AC5", agent)
    must("agent-join-held-flags-wired", "AC5", agent)
    must("schema-2885", "AC5", agent)
    must("still-running", "AC5", agent)
    must("deferred-cleanup", "AC5", agent)

    # AC6
    must("2945", "AC6", test)
    must("reservation-held", "AC6", test)
    must("mailbox-held", "AC6", test)
    must("check_join_held_flags_2945", "AC6", build)
    if (ROOT / "tests" / "orch" / "test_issue_2945.cpp").is_file():
        fails.append("AC6: test_issue_2945.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2945-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2945 Reclaimed join hash reservation-held + mailbox-held flags")
    return 0


if __name__ == "__main__":
    sys.exit(main())

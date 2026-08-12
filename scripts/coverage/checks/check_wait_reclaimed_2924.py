#!/usr/bin/env python3
"""Issue #2924: wait_reclaimed_body API after JoinStatus::Reclaimed.

AC:
  1. wait_reclaimed_body + WaitReclaimedResult in agent_spawn.h
  2. Timeout path preserves #2661 (no reservation release)
  3. Non-Reclaimed → Invalid; Done path cleanup once
  4. Metrics wait_reclaimed_{total,timeout,cleanup}_total + query keys
  5. Aura orch:agent-wait-reclaimed optional surface
  6. Extend test_join_drain_reclaim; no docs/design/; no invent test_issue_N
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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    spawn = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    fiber = _read("src/serve/fiber.h")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    must("Issue #2924" in spawn, "AC1: agent_spawn cites #2924")
    must("wait_reclaimed_body" in spawn, "AC1: wait_reclaimed_body")
    must("WaitReclaimedResult" in spawn, "AC1: WaitReclaimedResult")
    must("reclaimed_deferred_cleanup" in spawn, "AC1: deferred flag on AgentHandle")
    must("still_running_after_reclaim_counted" in spawn, "AC1: uses fiber still-running")
    must("still_running_after_reclaim_counted" in fiber, "AC1: fiber accessor present")

    # Timeout must not call Done-path cleanup (no release on residual).
    # Source-cite: return out before complete_agent_join_cleanup on Timeout.
    must("wait_reclaimed_timeout_total" in spawn, "AC2: timeout metric")
    must("JoinStatus::Timeout" in spawn and "wait_reclaimed_body" in spawn, "AC2: Timeout path")

    must("wait_reclaimed_cleanup_total" in spawn, "AC3: cleanup metric")
    must("JoinStatus::Invalid" in spawn, "AC3: Invalid path")

    must("wait_reclaimed_total" in spawn, "AC4: total metric")
    must("wait-reclaimed-total" in agent, "AC4: query key")
    must("wait-reclaimed-timeout-total" in agent, "AC4: query timeout key")
    must("wait-reclaimed-cleanup-total" in agent, "AC4: query cleanup key")
    must("schema-2924" in agent, "AC4: schema-2924")

    must("orch:agent-wait-reclaimed" in agent, "AC5: Aura surface")
    must("cleanup-completed" in agent, "AC5: cleanup-completed hash key")

    must("2924" in test and "wait_reclaimed_body" in test, "AC6: test extended")
    must("#2924 AC1" in test, "AC6: AC1 in test")
    must(
        "wait-reclaimed-2924" in build or "wait_reclaimed_2924" in build,
        "AC6: build.py",
    )
    must(not (ROOT / "tests/orch/test_issue_2924.cpp").is_file(), "AC6: no invent")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2924-*"):
            fails.append(f"AC6: docs/design/{f.name} forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2924 wait_reclaimed_body — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

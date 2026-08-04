#!/usr/bin/env python3
"""Issue #2401: agent-reply helper + orch:agent-reply Aura primitive.

Contract:
  AC1 agent_reply → agent_ask ok path
  AC2 unknown-corr / closed structured fail
  AC3 concurrent asks interleave-safe; no hang
  AC4 no AgentRegistry
  AC5 Aura prim + metrics + README + build gate

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

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test = _read("tests/orch/test_agent_ask.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2401", "AC1", spawn)
    must("agent_reply", "AC1", spawn)
    must("format_reply_payload", "AC1", spawn)
    must("g_pending_asks", "AC1", spawn)
    must("2401", "AC1", test)
    must("agent_reply", "AC1", test)

    # AC2
    must("unknown-corr", "AC2", spawn)
    must("agent_reply_fail_total", "AC2", spawn)
    must("unknown-corr", "AC2", test)

    # AC3 concurrent
    must("concurrent asks", "AC3", test)
    must("PendingAskGuard", "AC3", spawn)

    # AC4 no registry
    must_not("class AgentRegistry", "AC4", spawn)
    must("AC4: no class AgentRegistry", "AC4", test)

    # AC5
    must("orch:agent-reply", "AC5", prim)
    must("agent-reply-total", "AC5", prim)
    must("agent-reply-fail-total", "AC5", prim)
    must("schema-2401", "AC5", prim)
    must("agent-reply-wired", "AC5", prim)
    must("kAgentReplyIssue", "AC5", spawn)
    must("agent_reply", "AC5", readme)
    must("test_agent_ask", "AC5", cmake)
    must("tests/orch/test_agent_ask.cpp", "AC5", cmake)  # batch member source (S5)
    must("check_agent_reply_2401", "AC5", build)
    must("cmd_agent_reply_coverage", "AC5", build)
    must("AC5", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2401 agent-reply — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

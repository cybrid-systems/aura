#!/usr/bin/env python3
"""Issue #3051: Aura orch:agent-join / scope-join-all production auto short-wait.

Contract (one row per AC):
  AC1  Language surface only: production + Reclaimed + must_wait_reclaimed
       + caller did not pass :wait-reclaimed-ms → orch:agent-join and
       orch:scope-join-all call maybe_auto_wait_reclaimed_production /
       wait_reclaimed_body once with kProductionWaitReclaimedMsDefault.
       Soft / sandbox=off / must_wait=false: no extra wait.
  AC2  Explicit :wait-reclaimed-ms (incl. 0 = forever) wins; no double-wait.
  AC3  Hash stays authoritative: wait-reclaimed / wait-timeout / held
       flags + schema-3051 after the auto call.
  AC4  C++ join_agent / join_agents / JoinPolicy default unchanged (#3012).
  AC5  Reuse wait_reclaimed_* counters; no new process-global registry.
  AC6  Extend test_join_drain_reclaim (#81967); no test_issue_3051.cpp;
       no docs/design/ (#1655).

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

    spawn = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    scope = _read("src/orch/agent_scope.h")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # ── AC1: language helper + prims call it ──────────────────────
    must("Issue #3051", "AC1", spawn)
    must("maybe_auto_wait_reclaimed_production", "AC1", spawn)
    must("kProductionWaitReclaimedMsDefault", "AC1", spawn)
    must("maybe_auto_wait_reclaimed_production", "AC1", agent)
    if agent.count("maybe_auto_wait_reclaimed_production") < 2:
        fails.append("AC1: both orch:agent-join and orch:scope-join-all must call helper")

    # ── AC2: explicit override / no double-wait ───────────────────
    must("caller_passed_wait_reclaimed_ms", "AC2", spawn)
    must("wait_reclaimed_ms.has_value()", "AC2", agent)

    # ── AC3: hash keys ────────────────────────────────────────────
    must("schema-3051", "AC3", agent)
    must("join-auto-wait-reclaimed-wired", "AC3", agent)
    must("wait-reclaimed", "AC3", agent)
    must("wait-timeout", "AC3", agent)

    # ── AC4: C++ JoinPolicy default not injected ──────────────────
    must("#3051", "AC4", scope)
    ja = spawn.find("inline serve::JoinResult join_agent(AgentHandle& h, JoinPolicy policy)")
    jas = spawn.find("inline serve::JoinResult join_agents(std::span<AgentHandle> agents,\n")
    if ja < 0:
        fails.append("AC4: join_agent(JoinPolicy) missing")
    else:
        end = jas if jas > ja else ja + 4000
        if "maybe_auto_wait_reclaimed_production" in spawn[ja:end]:
            fails.append("AC4: join_agent must not call Aura auto-wait helper")
    if jas > 0:
        jas2 = spawn.find("inline serve::JoinResult join_agents(std::span<AgentHandle> agents,", jas + 1)
        end = jas2 if jas2 > jas else jas + 4000
        if "maybe_auto_wait_reclaimed_production" in spawn[jas:end]:
            fails.append("AC4: join_agents must not call Aura auto-wait helper")

    # ── AC5: reuse wait_reclaimed_* ───────────────────────────────
    must("wait_reclaimed_total", "AC5", spawn)
    must("schema-3051", "AC5", agent)
    if "AgentRegistry" in spawn[spawn.find("Issue #3051") : spawn.find("Issue #3051") + 900]:
        fails.append("AC5: #3051 must not introduce AgentRegistry")

    # ── AC6: tests + no invent + no docs/design/ ───────────────────
    must("#3051 AC1", "AC6", test)
    must("check_join_auto_wait_reclaimed_3051", "AC6", build)
    must("#3051", "AC6", spawn)
    for rel in ("tests/orch/test_issue_3051.cpp", "tests/compiler/test_issue_3051.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    if _read("docs/design/3051-auto-wait-reclaimed.md"):
        fails.append("AC6: docs/design/3051-auto-wait-reclaimed.md exists — forbidden per #1655")

    if fails:
        print(f"Issue #3051 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3051 Aura production auto short-wait — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

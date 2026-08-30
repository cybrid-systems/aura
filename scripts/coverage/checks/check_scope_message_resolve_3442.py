#!/usr/bin/env python3
"""Issue #3442: orch:agent-send/recv/ask/join resolve name-table then scope.

scope-spawn agents lived only in AgentScope::handles_. Message prims
consulted AgentNameTable only, so the documented multi-agent path
(orch:scope-spawn + orch:scope-watch) could not send. Resolve fallback
is session-local: name-table first, then AgentScope::find. No second
owning put, no process-global AgentRegistry, no new query key.

Contract:
  AC1 scope-spawn agent reachable by send/recv on the same Evaluator
  AC2 name-table spawn-agent unchanged (find first; one pointer miss)
  AC3 Soft/Off: same resolve, no new atomic on miss
  AC4 no AgentRegistry / no plane merge / no auto-put
  AC5 same-name: name-table wins (documented)
  AC6 no new query key; reuse existing observability
  AC7 extend existing orch tests; no test_issue_3442.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _block(hay: str, start_lit: str, end_lit: str) -> str:
    a = hay.find(start_lit)
    if a < 0:
        return ""
    b = hay.find(end_lit, a + 1) if end_lit else len(hay)
    return hay[a : b if b > a else len(hay)]


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    names = _read("src/compiler/agent_name_table.h")
    scope_h = _read("src/orch/agent_scope.h")
    readme = _read("src/orch/README.md")
    t_iso = _read("tests/orch/test_agent_name_table_isolation.cpp")
    t_scope = _read("tests/orch/test_agent_scope.cpp")
    t_orch = _read("tests/orch/test_orch_scope.cpp")
    build = _read("build.py")

    must("Issue #3442" in agent, "AC1: evaluator_primitives_agent.cpp cites #3442")
    must("resolve_aura_agent" in agent, "AC1: resolve_aura_agent helper")
    helper = _block(agent, "aura::orch::AgentHandle* resolve_aura_agent", "class WorkspaceSwapGuard")
    must("ev.agent_names_->find(name)" in helper, "AC2: helper name-table find")
    must("find_agent_scope" in helper, "AC1: helper scope fallback")
    must("scope->find(name)" in helper, "AC1: helper calls AgentScope::find")
    name_pos = helper.find("ev.agent_names_->find(name)")
    scope_pos = helper.find("find_agent_scope")
    must(
        name_pos >= 0 and scope_pos >= 0 and name_pos < scope_pos,
        "AC2: name-table find sits BEFORE scope find",
    )
    must("fetch_add" not in helper, "AC3: helper has no extra atomic on miss")

    join_b = _block(agent, 'add("orch:agent-join"', 'add("orch:agent-wait-reclaimed"')
    ask_b = _block(agent, 'add("orch:agent-ask"', 'add("orch:agent-reply"')
    send_b = _block(agent, 'add("orch:agent-send"', 'add("orch:agent-recv"')
    recv_b = _block(agent, 'add("orch:agent-recv"', 'add("orch:agent-touch"')
    must("resolve_aura_agent(ev, name)" in join_b, "AC1: agent-join uses resolve")
    must("resolve_aura_agent(ev, name)" in ask_b, "AC1: agent-ask uses resolve")
    must("resolve_aura_agent(ev, name)" in send_b, "AC1: agent-send uses resolve")
    must("resolve_aura_agent(ev, name)" in recv_b, "AC1: agent-recv uses resolve")

    spawn_b = _block(agent, 'add("orch:scope-spawn"', 'add("orch:scope-watch"')
    must("agent_names_->put" not in spawn_b, "AC5: scope-spawn must not auto-put")
    must("class AgentRegistry" not in agent, "AC4: no AgentRegistry in prims")
    must("class AgentRegistry" not in scope_h, "AC4: no AgentRegistry in agent_scope.h")
    must("never auto-puts scope handles" in names, "AC5: AgentNameTable documents no auto-put")
    must("name-table wins" in readme, "AC5: README documents name-table wins")
    must("#3442" in readme, "AC5: README cites #3442")
    must("schema-3442" not in agent, "AC6: no schema-3442 query key")
    must("g_3442_" not in agent, "AC6: no g_3442_* counter")
    must("check_scope_message_resolve_3442" in build, "AC7: build.py wires linter")
    must("3442 AC1" in t_orch, "AC7: test_orch_scope extended")
    must("3442 AC1" in t_scope, "AC7: test_agent_scope extended")
    must("3442 AC" in t_iso, "AC7: test_agent_name_table_isolation extended")
    must(not (ROOT / "tests/orch/test_issue_3442.cpp").is_file(), "AC7: no invent orch")
    must(not (ROOT / "tests/issues/test_issue_3442.cpp").is_file(), "AC7: no invent issues")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3442-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3442 scope_message_resolve:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3442 scope_message_resolve: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2926: session-local scope-resolve by name (no global AgentRegistry).

AC:
  1. AgentScope::find + orch:scope-resolve
  2. Miss → not-found; no global registry
  3. include-descendants hierarchy
  4. Metrics scope_resolve_total / miss + schema-2926
  5. MVP linter green (no AgentRegistry class)
  6. Extend test_orch_scope; no invent; no docs/design/
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

    scope_h = _read("src/orch/agent_scope.h")
    spawn_h = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_scope.cpp")
    build = _read("build.py")

    must("Issue #2926" in scope_h, "AC1: agent_scope cites #2926")
    must("find(std::string_view" in scope_h or "find(std::string_view name" in scope_h, "AC1: find API")
    must("find_unlocked_" in scope_h, "AC1: find_unlocked_")
    must("include_descendants" in scope_h, "AC3: include_descendants param")

    must("orch:scope-resolve" in agent, "AC1: Aura prim")
    must("not-found" in agent, "AC2: not-found status")
    must("scope_resolve_total" in spawn_h, "AC4: resolve_total metric")
    must("scope_resolve_miss_total" in spawn_h, "AC4: miss metric")
    must("schema-2926" in agent, "AC4: schema-2926")
    must("scope-resolve-total" in agent, "AC4: query key")

    must("class AgentRegistry" not in scope_h, "AC5: no AgentRegistry in agent_scope")
    must("class AgentRegistry" not in agent, "AC5: no AgentRegistry in agent prims")

    must("2926" in test and "scope-resolve" in test, "AC6: test extended")
    must("#2926 AC1" in test or "2926 AC1" in test, "AC6: AC1 in test")
    must("scope-resolve-2926" in build or "scope_resolve_2926" in build, "AC6: build.py")
    must(not (ROOT / "tests/orch/test_issue_2926.cpp").is_file(), "AC6: no invent")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2926-*"):
            fails.append(f"AC6: docs/design/{f.name} forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2926 scope-resolve — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

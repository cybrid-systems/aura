#!/usr/bin/env python3
"""Issue #3803: AgentScope N-agents ≠ concurrent mutate — join/workflow
surfaces isolation-level / region-key-missing from specs_ region_keys.

Contract:
  AC1  Production join/workflow hash: isolation-level + region-key-missing
       bool; Soft/single unchanged; existing counter OK; no new query key
  AC2  AgentSpec.region_key + orch:scope-spawn :region-key stamped;
       AgentScope::observe_isolation uses decide_isolation SSOT
  AC3  Soft/Off / single-agent / :pure unchanged
  AC4  No AgentRegistry / saga; extend existing tests; no invent

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
    scope = _read("src/orch/agent_scope.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")

    must("region_key = 0", "AC2 AgentSpec.region_key", spawn)
    must("kAgentScopeRegionKeyIsolationIssue = 3803", "AC1 stamp", scope)
    must("observe_isolation", "AC2 observe API", scope)
    must("ScopeIsolationObservation", "AC2 observation struct", scope)
    must("decide_isolation", "AC2 SSOT", scope)

    must("spec.region_key = region_key", "AC2 stamp into AgentSpec", agent)
    must("region-key-missing", "AC1 join/workflow bool", agent)
    must("schema-3803", "AC1 schema", agent)
    must("observe_isolation()", "AC1 join observe call", agent)

    must("Issue #3803", "AC1 README", readme)
    must("region-key-missing", "AC1 README surface", readme)
    must("ac3803_1_soft", "AC3 Soft test", test)
    must("ac3803_2_prod_missing", "AC1 prod missing test", test)
    must("ac3803_3_keys", "AC2 keys test", test)
    must("ac3803_4_single", "AC3 single-agent test", test)

    must("check_agent_scope_region_key_isolation_3803", "AC4 build.py", build)
    if "query:3803" in agent:
        fails.append("AC1: query:3803 invented (forbidden)")

    if (ROOT / "tests" / "orch" / "test_issue_3803.cpp").is_file():
        fails.append("AC4: tests/orch/test_issue_3803.cpp present (forbidden #81967)")
    if _read("docs/design/3803-scope-region-key.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")
    if "AgentRegistry" in agent and "no AgentRegistry" not in agent.lower() and "No AgentRegistry" not in agent:
        # soft — just ensure we didn't invent a registry type
        pass
    if "class AgentRegistry" in spawn or "class AgentRegistry" in scope:
        fails.append("AC4: invented AgentRegistry class")

    if fails:
        print("FAIL #3803 agent_scope_region_key_isolation:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3803 agent_scope_region_key_isolation: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2399: AgentScope concurrent access detection (metric + optional abort).

Contract:
  AC1 single-thread path — misuse counter stays 0 (re-entry ok)
  AC2 concurrent enter bumps agent_scope_concurrent_misuse_total
  AC3 AURA_AGENT_SCOPE_CONCURRENT_ABORT default OFF; ON hard abort
  AC4 additive query keys; no AgentRegistry / no internal mutex
  AC5 tests + build.py gate

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

    scope = _read("src/orch/agent_scope.h")
    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_agent_scope.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")
    mvp = _read("scripts/coverage/checks/check_orch_mvp_scope.py")

    # AC1 re-entry / single-thread
    must("Issue #2399", "AC1", scope)
    must("ScopeEnterGuard", "AC1", scope)
    must("enter_depth_", "AC1", scope)
    must("2399 AC1", "AC1", test)

    # AC2 concurrent metric
    must("agent_scope_concurrent_misuse_total", "AC2", scope)
    must("agent_scope_concurrent_misuse_total", "AC2", spawn)
    must("2399 AC2", "AC2", test)

    # AC3 abort env
    must("AURA_AGENT_SCOPE_CONCURRENT_ABORT", "AC3", scope)
    must("agent_scope_concurrent_abort_enabled", "AC3", scope)
    must("2399 AC3", "AC3", test)

    # AC4 additive + no registry / no mutex lock
    must("agent-scope-concurrent-misuse-total", "AC4", prim)
    must("schema-2399", "AC4", prim)
    must("issue-2399", "AC4", prim)
    must("agent-scope-concurrent-detect-wired", "AC4", prim)
    must("kAgentScopeConcurrentMisuseIssue", "AC4", spawn)
    must_not("std::mutex", "AC4", scope)
    # No process-global registry *definition* reintro (doc comments may name them).
    must_not("class AgentRegistry", "AC4", scope)
    must_not("static AgentRegistry", "AC4", scope)
    must("2399 AC4", "AC4", test)

    # AC5
    must("2399 AC5", "AC5", test)
    must("check_agent_scope_concurrent_2399", "AC5", build)
    must("cmd_agent_scope_concurrent_coverage", "AC5", build)
    must("test_agent_scope", "AC5", cmake)
    # MVP scope linter still forbids process-global registry identifiers.
    must("AgentRegistry", "AC5", mvp)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2399 AgentScope concurrent detect — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2782: AgentScope borrowed Scheduler lifetime (no UAF).

AgentScope held Scheduler&; if Scheduler was destroyed first, sched_ and
Fiber* dangled. Fix: nullable Scheduler*, register opaque observer on
Scheduler, ~Scheduler notifies before fiber teardown, ops fail-closed.

Contract (one row per AC):
  AC1 Scheduler register/unregister_agent_scope_observer + dtor notify
  AC2 AgentScope stores Scheduler*, observer nulls fibers, scheduler_alive
  AC3 agent_scope_scheduler_invalidated/dangling totals + schema-2782
  AC4 ac2782_* tests in test_agent_scope; no test_issue_2782.cpp
  AC5 this linter wired; no docs/design/2782-*

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

    sched_h = _read("src/serve/scheduler.h")
    sched_c = _read("src/serve/scheduler.cpp")
    scope = _read("src/orch/agent_scope.h")
    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_agent_scope.cpp")
    build = _read("build.py")

    # AC1
    must("register_agent_scope_observer", "AC1", sched_h)
    must("unregister_agent_scope_observer", "AC1", sched_h)
    must("ScopeLifetimeFn", "AC1", sched_h)  # nested typedef on Scheduler
    must("register_agent_scope_observer", "AC1", sched_c)
    must("2782", "AC1", sched_c)

    # AC2
    must("kAgentScopeSchedulerLifetimeIssue", "AC2", scope)
    must("scheduler_alive", "AC2", scope)
    must("on_scheduler_destroyed_", "AC2", scope)
    must("serve::Scheduler* sched_", "AC2", scope)
    must_not("serve::Scheduler& sched_", "AC2", scope)

    # AC3
    must("agent_scope_scheduler_invalidated_total", "AC3", spawn)
    must("agent_scope_scheduler_dangling_total", "AC3", spawn)
    must("schema-2782", "AC3", prim)
    must("agent-scope-scheduler-lifetime-wired", "AC3", prim)

    # AC4
    must("ac2782_scheduler_destroyed_before_scope", "AC4", test)
    must("ac2782_source_and_query", "AC4", test)
    if (ROOT / "tests" / "orch" / "test_issue_2782.cpp").is_file():
        fails.append("AC4: test_issue_2782.cpp present (forbidden per #81967)")

    # AC5
    must("check_agent_scope_scheduler_lifetime_2782", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2782-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2782 AgentScope Scheduler lifetime — observer + fail-closed + schema-2782")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2946: production default AgentScope concurrent hard deny.

Refine #2399 — production_defaults_active → HardDeny (structured fail);
Soft / AURA_SANDBOX=off stays metric-only; env=0 opt-out; env=1 HardAbort.

Contract (one row per AC):
  AC1  production HardDeny + spawn denied_hard skips handles_ mutation
  AC2  Soft / sandbox=off / env=0 → SoftMetric
  AC3  single-thread re-entry path unchanged (no hard deny)
  AC4  hierarchy cancel_all_unlocked_ preserved (#2781)
  AC5  schema-2946 + hard-deny total query key; #2399 preserved
  AC6  tests + build.py; no invent/design

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

    # AC1
    must("Issue #2946", "AC1", scope)
    must("HardDeny", "AC1", scope)
    must("resolve_agent_scope_concurrent_policy", "AC1", scope)
    must("production_defaults_active", "AC1", scope)
    must("denied_hard", "AC1", scope)
    must("agent_scope_concurrent_hard_deny_total", "AC1", spawn)
    must("concurrent hard deny", "AC1", scope)
    must("2946 AC1", "AC1", test)

    # AC2
    must("AURA_AGENT_SCOPE_CONCURRENT_ABORT", "AC2", scope)
    must("SoftMetric", "AC2", scope)
    must("AURA_SANDBOX", "AC2", scope)
    must("2946 AC2", "AC2", test)

    # AC3
    must("enter_depth_", "AC3", scope)
    must("2946 AC3", "AC3", test)

    # AC4
    must("cancel_all_unlocked_", "AC4", scope)
    must("2781", "AC4", scope)

    # AC5
    must("schema-2946", "AC5", prim)
    must("agent-scope-concurrent-hard-deny-total", "AC5", prim)
    must("agent-scope-concurrent-hard-deny-wired", "AC5", prim)
    must("kAgentScopeConcurrentHardDenyIssue", "AC5", spawn)
    must("schema-2399", "AC5", prim)
    must("agent_scope_concurrent_misuse_total", "AC5", spawn)

    # AC6
    must("ac2946", "AC6", test)
    must("check_agent_scope_concurrent_hard_deny_2946", "AC6", build)
    must_not("std::mutex", "AC6", scope)
    must_not("class AgentRegistry", "AC6", scope)
    if (ROOT / "tests" / "orch" / "test_issue_2946.cpp").is_file():
        fails.append("AC6: test_issue_2946.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2946-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2946 production AgentScope concurrent hard deny default")
    return 0


if __name__ == "__main__":
    sys.exit(main())

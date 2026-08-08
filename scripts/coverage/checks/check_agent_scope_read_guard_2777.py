#!/usr/bin/env python3
"""Issue #2777: AgentScope read APIs take ScopeEnterGuard (#2399 residual).

directory_snapshot / handles / child_at / size / empty bypassed concurrent
misuse detection. Concurrent ~AgentScope + directory walk is silent UB.

Contract (one row per AC):
  AC1 directory_snapshot / handles / child_at / size / empty use ScopeEnterGuard
  AC2 directory_snapshot_concurrent_total distinct metric on concurrent enter
  AC3 child merge under guard; no std::mutex / no AgentRegistry reintro
  AC4 schema-2777 + query keys; ac2777_* tests
  AC5 this linter wired; no docs/design/*

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

    # AC1 — read APIs guarded
    must("#2777", "AC1", scope)
    must("kAgentScopeReadGuardIssue", "AC1", scope)
    for site in (
        "directory_snapshot",
        "handles",
        "handles_mut",
        "child_at",
        "size",
        "empty",
        "parent",
        "is_root",
        "child_count",
    ):
        must(f'"{site}"', "AC1", scope)
    must('ScopeEnterGuard g(this, "directory_snapshot")', "AC1", scope)

    # AC2 — concurrent total
    must("directory_snapshot_concurrent_total", "AC2", scope)
    must("directory_snapshot_concurrent_total", "AC2", spawn)
    must("directory-snapshot-concurrent-total", "AC2", prim)

    # AC3 — child walk under guard; no mutex lock
    must("merge_directory_under_guard_", "AC3", scope)
    must_not("std::mutex", "AC3", scope)
    must_not("class AgentRegistry", "AC3", scope)

    # AC4 — tests + schema
    must("ac2777_read_apis_guarded", "AC4", test)
    must("schema-2777", "AC4", prim)
    must("agent-scope-read-guard-wired", "AC4", prim)
    must("2399", "AC4", scope)  # lineage

    # AC5
    must("check_agent_scope_read_guard_2777", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2777-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_2777.cpp").is_file():
        fails.append("AC5: test_issue_2777.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2777 AgentScope read APIs ScopeEnterGuard — directory_snapshot concurrent detect + schema-2777")
    return 0


if __name__ == "__main__":
    sys.exit(main())

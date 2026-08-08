#!/usr/bin/env python3
"""Issue #2781: hierarchy cancel_all must not false-positive #2399 misuse.

Parent cancel_all recursed via c->cancel_all() which re-entered
ScopeEnterGuard on each child. That polluted agent_scope_concurrent_misuse_total
and risked AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 false aborts.

Contract (one row per AC):
  AC1 cancel_all uses cancel_all_unlocked_ for child walk (no c->cancel_all())
  AC2 agent_scope_hierarchy_cancel_total + kAgentScopeHierarchyCancelIssue
  AC3 ac2781_* tests in test_agent_scope_hierarchy; schema-2781 query keys
  AC4 #2399 misuse path still present (direct concurrent still detected)
  AC5 this linter wired; no docs/design/2781-*; no test_issue_2781.cpp

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
    test = _read("tests/orch/test_agent_scope_hierarchy.cpp")
    build = _read("build.py")

    # AC1 — unlocked hierarchy walk
    must("kAgentScopeHierarchyCancelIssue", "AC1", scope)
    must("2781", "AC1", scope)
    must("cancel_all_unlocked_", "AC1", scope)
    must("from_hierarchy", "AC1", scope)
    must_not("c->cancel_all()", "AC1", scope)

    # AC2 — metric
    must("agent_scope_hierarchy_cancel_total", "AC2", spawn)
    must("agent_scope_hierarchy_cancel_total", "AC2", scope)

    # AC3 — tests + query
    must("ac2781_hierarchy_cancel_no_misuse", "AC3", test)
    must("ac2781_stress_deep_cancel", "AC3", test)
    must("ac2781_source_and_query", "AC3", test)
    must("schema-2781", "AC3", prim)
    must("agent-scope-hierarchy-cancel-total", "AC3", prim)
    must("agent-scope-hierarchy-cancel-wired", "AC3", prim)

    # AC4 — #2399 path retained
    must("agent_scope_concurrent_misuse_total", "AC4", scope)
    must("ScopeEnterGuard", "AC4", scope)
    must("try_enter", "AC4", scope)

    # AC5
    must("check_agent_scope_hierarchy_cancel_2781", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2781-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_2781.cpp").is_file():
        fails.append("AC5: test_issue_2781.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2781 hierarchy cancel unlocked walk — no #2399 false-positive, schema-2781 + hierarchy_cancel_total"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

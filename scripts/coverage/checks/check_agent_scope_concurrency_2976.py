#!/usr/bin/env python3
"""Issue #2976: AgentScope SingleOwner (default) vs MutexGuarded.

Contract (one row per AC):
  AC1 Default SingleOwner; existing concurrent misuse still counted.
  AC2 MutexGuarded: concurrent spawn without misuse bump.
  AC3 MutexGuarded join_all / cancel_all under concurrent spawn; #2782 preserved.
  AC4 Soft / unit: default still SingleOwner zero-lock.
  AC5 Extend test_agent_scope; coverage linter; no docs/design/* (#1655).
  AC6 MVP scope linter green (no AgentRegistry).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    scope = _read("src/orch/agent_scope.h")
    spawn = _read("src/orch/agent_spawn.h")
    q = _read("src/compiler/evaluator_primitives_agent.cpp")
    t = _read("tests/orch/test_agent_scope.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    # AC1 — default SingleOwner.
    must("kAgentScopeConcurrencyIssue = 2976", "AC1", scope)
    must("enum class ScopeConcurrency", "AC1", scope)
    must("SingleOwner = 0", "AC1", scope)
    must("struct AgentScopeOptions", "AC1", scope)
    must("AgentScopeOptions opts = {}", "AC1", scope)
    must("ac2976_1_default_single_owner", "AC1", t)
    must("agent_scope_concurrent_misuse_total", "AC1 preserved", scope)

    # AC2 — MutexGuarded concurrent spawn.
    must("MutexGuarded = 1", "AC2", scope)
    must("recursive_mutex", "AC2", scope)
    must("agent_scope_mutex_guarded_enter_total", "AC2", spawn)
    must("ac2976_2_mutex_guarded_concurrent_spawn", "AC2", t)
    must("no misuse bump", "AC2", t)

    # AC3 — join/cancel under concurrent spawn.
    must("ac2976_3_mutex_guarded_join_cancel_concurrent", "AC3", t)
    must("#2782", "AC3", t)
    must("parent-before-child", "AC3 hierarchy", scope)

    # AC4 — Soft / unit default zero-lock.
    must("zero lock", "AC4", scope)
    must("ac2976_4_soft_unit_zero_lock", "AC4", t)
    must("Taken only when mode_ == MutexGuarded", "AC4", scope)

    # AC5 — tests + linter + README + no design/invent.
    must("ac2976_1_default_single_owner", "AC5", t)
    must("ac2976_2_mutex_guarded_concurrent_spawn", "AC5", t)
    must("ac2976_3_mutex_guarded_join_cancel_concurrent", "AC5", t)
    must("ac2976_4_soft_unit_zero_lock", "AC5", t)
    must("ac2976_5_source_linter", "AC5", t)
    must("ac2976_6_mvp", "AC5", t)
    must("check_agent_scope_concurrency_2976", "AC5", build)
    must("2976", "AC5", readme)
    must("MutexGuarded", "AC5", readme)
    must_key("schema-2976", "AC5", q)
    must_key("agent-scope-concurrency-wired", "AC5", q)
    must_key("agent-scope-mutex-guarded-enter-total", "AC5", q)
    must_key("schema-2399", "AC5 preserved", q)
    if (ROOT / "tests" / "orch" / "test_issue_2976.cpp").is_file():
        fails.append("AC5: test_issue_2976.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2976*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — MVP scope.
    if "class AgentRegistry" in scope:
        fails.append("AC6: class AgentRegistry in agent_scope.h")
    if "conduct_parallel(" in scope:
        fails.append("AC6: conduct_parallel( in agent_scope.h")
    mvp = ROOT / "scripts" / "coverage" / "checks" / "check_orch_mvp_scope.py"
    if mvp.is_file():
        r = subprocess.run(
            [sys.executable, str(mvp)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"AC6: check_orch_mvp_scope.py regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2976 AgentScope SingleOwner / MutexGuarded — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Issue #2756: WorkflowFailurePolicy composition (parallel_intend + AgentScope).

Contract (one row per AC):
  AC1 Composition helper maps workflow policy onto ParallelPolicy +
     AgentFailurePolicy without changing defaults when unused.
  AC2 Residual / Reclaimed path can be observed under the workflow policy
     without violating the #2661 cleanup contract.
  AC3 Additive metrics + schema/issue sentinels on query:orch-module-stats.
  AC4 Tests cover FailFast→Cancel, RetryN→RestartN, CircuitBreaker→Cancel,
     residual observation; extend test_failure_policy_bridge.
  AC5 README short section; source-cite; no docs/design/2756-*.
  AC6 MVP scope linter still green (no AgentRegistry / global registry).

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

    hdr = _read("src/orch/agent_spawn.h")
    q = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    t = _read("tests/orch/test_failure_policy_bridge.cpp")
    build = _read("build.py")

    # AC1 — composition helper.
    must("Issue #2756", "AC1", hdr)
    must("struct WorkflowFailurePolicy", "AC1", hdr)
    must("compose_workflow_policy", "AC1", hdr)
    must("to_parallel_policy", "AC1", hdr)
    must("ResidualReclaimPreference", "AC1", hdr)
    must("kWorkflowFailurePolicyIssue = 2756", "AC1", hdr)
    # Defaults unchanged when unused (documented).
    must("when the helper is not used", "AC1", hdr)

    # AC2 — residual observe; #2661 preserved.
    must("note_workflow_residual_reclaim_under_policy", "AC2", hdr)
    must("residual_prefers_cancel", "AC2", hdr)
    must("residual_prefers_defer", "AC2", hdr)
    must("complete_agent_join_cleanup", "AC2", hdr)
    must("join_reclaimed_deferred_cleanup_total", "AC2", hdr)
    must("#2661", "AC2", hdr)

    # AC3 — query keys.
    must("Issue #2756", "AC3", q)
    must_key("workflow-compose-total", "AC3", q)
    must_key("workflow-retry-total", "AC3", q)
    must_key("workflow-circuit-open-total", "AC3", q)
    must_key("workflow-residual-reclaim-under-policy-total", "AC3", q)
    must_key("workflow-failure-policy-wired", "AC3", q)
    must('"schema-2756"', "AC3", q)
    must('"issue-2756"', "AC3", q)
    must("workflow_compose_total", "AC3", hdr)
    must("workflow_failure_policy_wired", "AC3", hdr)
    # Prior surfaces preserved.
    must('"schema-2229"', "AC3", q)
    must("agent-failure-policy-wired", "AC3", q)

    # AC4 — tests in existing bridge suite.
    must("#2756 AC1", "AC4", t)
    must("#2756 AC2", "AC4", t)
    must("#2756 AC4", "AC4", t)
    must("FailFast → Cancel", "AC4", t)
    must("RetryN → RestartN", "AC4", t)
    must("CircuitBreaker → Cancel", "AC4", t)
    must("compose_workflow_policy", "AC4", t)
    must("note_workflow_residual_reclaim_under_policy", "AC4", t)

    # AC5 — README + source-cite + no docs/design.
    must("2756", "AC5", readme)
    must("WorkflowFailurePolicy", "AC5", readme)
    must("compose_workflow_policy", "AC5", readme)
    must("check_workflow_failure_policy_2756", "AC5", build)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2756-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — MVP scope (no registry reintro). Comments may mention the
    # forbidden names as documentation; rely on check_orch_mvp_scope.py
    # (comment/string-stripping) for the real reintroduction guard.
    mvp = ROOT / "scripts" / "coverage" / "checks" / "check_orch_mvp_scope.py"
    if mvp.is_file():
        r = subprocess.run(
            [sys.executable, str(mvp), "--strict", "--quiet"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"AC6: check_orch_mvp_scope.py --strict failed:\n{r.stdout}\n{r.stderr}")
    # No class AgentRegistry definition in composition surface.
    if "class AgentRegistry" in hdr:
        fails.append("AC6: class AgentRegistry definition reintroduced in agent_spawn.h")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2756 WorkflowFailurePolicy composition — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

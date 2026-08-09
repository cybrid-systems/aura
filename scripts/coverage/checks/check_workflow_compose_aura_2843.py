#!/usr/bin/env python3
"""Issue #2843: Aura language surface for WorkflowFailurePolicy (#2756 residual).

  AC1 orch:compose-workflow maps FailFast/CollectAll/RetryN/CircuitBreaker
     onto agent stall policy fields (parity with C++ to_agent_policy)
  AC2 residual preference advisory only (#2661 reclaim unchanged)
  AC3 schema-2756 lineage + schema-2843; Soft never denies
  AC4 compose projects parallel-intend + scope-watch kwargs; :workflow apply
  AC5 extend test_failure_policy_bridge; coverage linter; no docs/design/
  AC6 MVP scope linter green (no AgentRegistry / conduct_parallel)

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

    # AC1 — prim + mapping helpers
    must("kWorkflowComposeAuraIssue", "AC1", hdr)
    must("2843", "AC1", hdr)
    must("workflow_compose_aura_total", "AC1", hdr)
    must("note_workflow_compose_aura", "AC1", hdr)
    must("failure_policy_name", "AC1", hdr)
    must("agent_failure_action_name", "AC1", hdr)
    must("orch:compose-workflow", "AC1", q)
    must("compose_workflow_policy", "AC1", q)

    # AC2 — residual advisory
    must("residual-cancel", "AC2", q)
    must("residual-defer", "AC2", q)
    must("advisory", "AC2", q)

    # AC3 — schema lineage
    must_key("schema-2756", "AC3", q)
    must_key("schema-2843", "AC3", q)
    must_key("issue-2843", "AC3", q)
    must_key("workflow-compose-aura-total", "AC3", q)

    # AC4 — kwargs projection + :workflow apply
    must("parallel-intend-kwargs-ready", "AC4", q)
    must("scope-watch-kwargs-ready", "AC4", q)
    must("workflow-policy", "AC4", q)
    must("Issue #2843", "AC4", q)

    # AC5 — tests + linter + no invent/design
    must("ac2843_1_compose_parity_with_cpp", "AC5", t)
    must("ac2843_2_residual_advisory_only", "AC5", t)
    must("ac2843_3_schema_and_soft", "AC5", t)
    must("ac2843_4_project_kwargs_for_prims", "AC5", t)
    must("ac2843_5_source_linter_mvp", "AC5", t)
    must("check_workflow_compose_aura_2843", "AC5", build)
    must("orch:compose-workflow", "AC5", readme)
    must("2843", "AC5", readme)
    if (ROOT / "tests" / "orch" / "test_issue_2843.cpp").is_file():
        fails.append("AC5: test_issue_2843.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2843*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — MVP scope (no AgentRegistry / conduct_parallel reintro)
    if "class AgentRegistry" in hdr:
        fails.append("AC6: class AgentRegistry in agent_spawn.h")
    if "conduct_parallel(" in hdr:
        fails.append("AC6: conduct_parallel( in agent_spawn.h")
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
    print("OK: Issue #2843 Aura orch:compose-workflow surface — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

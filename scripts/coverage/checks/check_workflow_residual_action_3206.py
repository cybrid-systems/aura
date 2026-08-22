#!/usr/bin/env python3
"""Issue #3206: apply_workflow residual Cancel/JoinDrain under production.

Phase C was observe-only. Production + explicit CancelOnResidual /
JoinDrainOnResidual now cancel_all + short join_all (existing join
path; #2661 no early free). Soft / Report / Defer stay observe-only.

Contract:
  AC1 Soft / unset Report: zero extra action
  AC2 production + CancelOnResidual → cancel_all
  AC3 JoinDrainOnResidual + residual-action hash; no global registry
  AC4 extend test_failure_policy_bridge + 2756/2843 linters; no invent

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
    q = _read("src/compiler/evaluator_primitives_agent.cpp")
    t = _read("tests/orch/test_failure_policy_bridge.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    must("kWorkflowResidualActionIssue = 3206", "AC1 stamp", spawn)
    must("CancelOnResidual", "AC1 enum", spawn)
    must("JoinDrainOnResidual", "AC1 join-drain enum", spawn)
    must("kResidualJoinDrainMs", "AC1 short drain", spawn)
    must("ac3206_1_soft_quiet", "AC1 Soft test", t)
    must("production_defaults_active()", "AC1 production gate", scope)

    must("apply_residual_reclaim_action", "AC2 helper", scope)
    must("scope.cancel_all()", "AC2 cancel_all", scope)
    must("ac3206_2_prod_cancel", "AC2 test", t)
    must("workflow_residual_cancel_total", "AC2 counter", spawn)

    must("residual-action", "AC3 hash", q)
    must("schema-3206", "AC3 schema", q)
    must("workflow-residual-join-drain-total", "AC3 query key", q)
    must("ac3206_3_join_drain", "AC3 test", t)
    must("3206", "AC3 README", readme)
    if "class AgentRegistry" in spawn:
        fails.append("AC3: AgentRegistry class")

    must("check_workflow_residual_action_3206", "AC4 build.py", build)
    must("ac3206_5_source_linter", "AC4 test", t)
    must("complete_agent_join_cleanup", "AC4 #2661 still cited", spawn)
    if (ROOT / "tests" / "orch" / "test_issue_3206.cpp").is_file():
        fails.append("AC4: tests/orch/test_issue_3206.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3206-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3206 workflow residual action:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3206 workflow residual action: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

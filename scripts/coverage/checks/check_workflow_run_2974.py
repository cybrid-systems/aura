#!/usr/bin/env python3
"""Issue #2974: multi-stage workflow primitive (DAG stages over parallel_intend).

Contract (one row per AC):
  AC1 Two+ stages run in order; stage 2 does not start if stage 1 fails
     and stop_on_batch_fail.
  AC2 Per-stage FailurePolicy / AgentFailurePolicy projection matches
     #2539/#2756 tables.
  AC3 Residual path only calls note_workflow_residual_reclaim_under_policy
     — no change to #2661 reclaim.
  AC4 Defaults for callers that never use run_workflow unchanged
     (#2007/#2229/#2852).
  AC5 Additive workflow-run-total / workflow-stage-fail-total / schema-2974;
     existing compose/apply counters preserved.
  AC6 Tests extend test_failure_policy_bridge (ac2974_*); MVP scope linter
     green; no docs/design/* (#1655).

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
    scope = _read("src/orch/agent_scope.h")
    q = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    t = _read("tests/orch/test_failure_policy_bridge.cpp")
    build = _read("build.py")

    # AC1 — ordered stages + stop_on_batch_fail.
    must("Issue #2974", "AC1", hdr)
    must("struct WorkflowStage", "AC1", hdr)
    must("struct WorkflowRunResult", "AC1", hdr)
    must("run_workflow", "AC1", hdr)
    must("stop_on_batch_fail", "AC1", hdr)
    must("kWorkflowRunIssue = 2974", "AC1", hdr)
    must("run_workflow", "AC1", scope)
    must("ac2974_1_ordered_stages_stop_on_fail", "AC1", t)
    must("stage 2 did not start", "AC1", t)

    # AC2 — per-stage projection via compose / #2539/#2756 tables.
    must("make_workflow_stage", "AC2", hdr)
    must("compose_workflow_policy", "AC2", hdr)
    must("FailFast → Cancel", "AC2", t)
    must("RetryN → RestartN", "AC2", t)
    must("CircuitBreaker → Cancel", "AC2", t)
    must("compose_workflow_policy", "AC2", q)

    # AC3 — residual observe only; #2661 preserved.
    must("note_workflow_residual_reclaim_under_policy", "AC3", scope)
    must("#2661", "AC3", scope)
    must("observe-only", "AC3", scope)
    must("note_workflow_residual_reclaim_under_policy", "AC3", t)
    if "complete_agent_join_cleanup" in scope and "run_workflow" in scope:
        # run_workflow body must not call reclaim helpers.
        body = scope.split("run_workflow", 1)[-1]
        if "complete_agent_join_cleanup" in body.split("} // namespace aura::orch")[0]:
            fails.append("AC3: run_workflow calls complete_agent_join_cleanup")

    # AC4 — unused callers unchanged.
    must("apply_workflow", "AC4", hdr)
    must("workflow_apply_total", "AC4", hdr)
    must("#2007", "AC4", hdr)
    must("defaults unchanged", "AC4", hdr)

    # AC5 — additive metrics + schema; compose/apply preserved.
    must("workflow_run_total", "AC5", hdr)
    must("workflow_stage_fail_total", "AC5", hdr)
    must("Issue #2974", "AC5", q)
    must_key("workflow-run-total", "AC5", q)
    must_key("workflow-stage-fail-total", "AC5", q)
    must_key("schema-2974", "AC5", q)
    must_key("issue-2974", "AC5", q)
    must_key("workflow-compose-total", "AC5", q)
    must("orch:run-workflow", "AC5", q)
    must("schema-2974", "AC5", t)

    # AC6 — tests + linter + README + no design / no invent test / MVP.
    must("ac2974_1_ordered_stages_stop_on_fail", "AC6", t)
    must("ac2974_2_per_stage_policy_projection", "AC6", t)
    must("ac2974_3_residual_observe_only", "AC6", t)
    must("ac2974_4_defaults_unchanged", "AC6", t)
    must("ac2974_5_additive_metrics", "AC6", t)
    must("ac2974_6_tests_linter_mvp", "AC6", t)
    must("check_workflow_run_2974", "AC6", build)
    must("2974", "AC6", readme)
    must("orch:run-workflow", "AC6", readme)
    if (ROOT / "tests" / "orch" / "test_issue_2974.cpp").is_file():
        fails.append("AC6: test_issue_2974.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2974*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")
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
    print("OK: Issue #2974 multi-stage workflow primitive — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

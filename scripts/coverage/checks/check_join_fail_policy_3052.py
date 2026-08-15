#!/usr/bin/env python3
"""Issue #3052: AgentFailurePolicy::on_join_fail wired in join_all.

Contract (one row per AC):
  AC1  join_all honors on_join_fail (RestartN / Cancel / Throttle /
       ReportOnly) after per-handle last_join_status.
  AC2  Reclaimed + deferred / still-running is not restart fuel (#2661).
  AC3  Default on_join_fail stays ReportOnly.
  AC4  to_agent_policy(RetryN) sets on_join_fail=RestartN; no silent
       override of an explicit AgentFailurePolicy.
  AC5  Extend test_agent_failure_policy + test_failure_policy_bridge
       (#81967); no test_issue_3052.cpp; no docs/design/ (#1655).

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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    t_pol = _read("tests/orch/test_agent_failure_policy.cpp")
    t_br = _read("tests/orch/test_failure_policy_bridge.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    must("Issue #3052", "AC1", scope)
    must("apply_on_join_fail_unlocked_", "AC1", scope)
    must("last_join_status", "AC1", spawn)
    must("on_join_fail", "AC1", scope)

    must("reclaimed_deferred_cleanup", "AC2", scope)
    must("#2661", "AC2", scope)

    must("on_join_fail = AgentFailureAction::ReportOnly", "AC3", spawn)

    must("on_join_fail = AgentFailureAction::RestartN", "AC4", spawn)
    must("#3052 AC4", "AC4", t_br)
    must("explicit AgentFailurePolicy not overwritten", "AC4", t_br)

    must("#3052 AC1", "AC5", t_pol)
    must("#3052 AC2", "AC5", t_pol)
    must("#3052 AC3", "AC5", t_pol)
    must("schema-3052", "AC5", agent)
    must("3052", "AC5", readme)
    must("check_join_fail_policy_3052", "AC5", build)
    if _read("tests/orch/test_issue_3052.cpp") or _read("tests/compiler/test_issue_3052.cpp"):
        fails.append("AC5: test_issue_3052.cpp exists — forbidden per #81967")
    if _read("docs/design/3052-on-join-fail.md"):
        fails.append("AC5: docs/design/3052-* exists — forbidden per #1655")
    if "AgentRegistry" in spawn[spawn.find("Issue #3052") : spawn.find("Issue #3052") + 800]:
        fails.append("AC5: #3052 must not introduce AgentRegistry")

    if fails:
        print(f"Issue #3052 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3052 join_all on_join_fail — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

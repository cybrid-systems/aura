#!/usr/bin/env python3
"""Issue #3208: production default on_join_fail Cancel when unset.

#3052 wired join_all on_join_fail but the default stayed ReportOnly.
Production + unset now injects Cancel. Soft / explicit ReportOnly
unchanged. Reclaimed still-running still skip (#2661).

Contract:
  AC1 Soft / explicit ReportOnly: zero extra action
  AC2 production + unset Timeout/Cancelled → Cancel path
  AC3 Reclaimed still-running skip (#2661)
  AC4 orch:scope-join-all hash on-join-fail-effective /
      join-fail-action-taken; additive cancel counter
  AC5 extend test_agent_failure_policy; no invent; no AgentRegistry

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
    t = _read("tests/orch/test_agent_failure_policy.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    must("kJoinFailProductionDefaultIssue = 3208", "AC4 stamp", spawn)
    must("agent_join_fail_action_cancel_total", "AC4 cancel counter", spawn)
    must("on_join_fail = AgentFailureAction::ReportOnly", "AC1 struct default", spawn)

    must("resolve_on_join_fail", "AC2 resolver", scope)
    must("production_defaults_active()", "AC2 production gate", scope)
    must("AURA_JOIN_FAIL_ACTION", "AC2 env", scope)
    must("request_cancel()", "AC2 Cancel path", scope)
    must("reclaimed_deferred_cleanup", "AC3 #2661 skip", scope)
    must("#2661", "AC3 cite", scope)
    must("std::optional<AgentFailurePolicy>", "AC1 unset vs explicit", scope)

    must("on-join-fail-effective", "AC4 hash", q)
    must("join-fail-action-taken", "AC4 hash taken", q)
    must("schema-3208", "AC4 schema", q)
    must("agent-join-fail-action-cancel-total", "AC4 query key", q)
    must("join-fail-production-default-wired", "AC4 wired", q)

    must("ac3208_1_soft_explicit_report_only", "AC1 test", t)
    must("ac3208_2_prod_unset_cancel", "AC2 test", t)
    must("ac3208_3_reclaimed_skip", "AC3 test", t)
    must("ac3208_4_hash_and_soak", "AC4/AC5 test", t)
    must("HardDeny", "AC5 HardDeny cite", t)

    must("3208", "AC4 README", readme)
    must("check_join_fail_production_default_3208", "AC5 build.py", build)
    if "class AgentRegistry" in spawn:
        fails.append("AC5: AgentRegistry class")
    if (ROOT / "tests" / "orch" / "test_issue_3208.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3208.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3208-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3208 production join_fail default:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3208 production join_fail default: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

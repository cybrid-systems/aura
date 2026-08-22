#!/usr/bin/env python3
"""Issue #3251: unified deny-class on Aura spawn/join/send fail hashes.

Contract (one row per AC):
  AC1  Soft / success: production_defaults gate, no intern on quiet path
  AC2  production spawn quota / BP / schedule-gate / try-acquire / handoff
       / closed distinguishable via deny-class
  AC3  spawn quota reject still finalize_spawn_quota_reject (#2155)
  AC4  no old query key rewrite; module-stats additive schema-3251
  AC5  tests in existing suites; no test_issue_3251.cpp; no docs/design/

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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    scope = _read("src/orch/agent_scope.h")
    readme = _read("src/orch/README.md")
    t_obs = _read("tests/orch/test_orch_obs_facade.cpp")
    t_bp = _read("tests/orch/test_per_scope_bp_admit.cpp")
    t_q = _read("tests/serve/test_spawn_quota_no_leak.cpp")
    t_ss = _read("tests/orch/test_security_schedule_gate.cpp")
    build = _read("build.py")

    must("kAgentDenyClassIssue = 3251", "AC1 stamp", spawn)
    must("classify_agent_deny", "AC1 classify", spawn)
    must("production_defaults_active()", "AC1 Soft gate", agent)
    must("deny-class", "AC1 Aura intern", agent)

    must("AgentDenyClass::Quota", "AC2 quota stamp", spawn)
    must("AgentDenyClass::BpAdmit", "AC2 bp stamp", spawn)
    must("AgentDenyClass::ScheduleGate", "AC2 schedule stamp", spawn)
    must("admit_security_schedule", "AC2 body schedule", fib)
    must("AgentDenyClass::Handoff", "AC2 send handoff", agent)
    must("AgentDenyClass::Closed", "AC2 send closed", agent)
    must("body-not-run", "AC2 join lifecycle", agent)
    must("body-not-run", "AC2 directory lifecycle", scope)

    must("finalize_spawn_quota_reject", "AC3 no-leak", spawn)
    must("schema-3251", "AC4 query additive", agent)
    must("agent-deny-class-wired", "AC4 wired", agent)
    must("deny-class", "AC4 README", readme)

    must("3251", "AC5 obs test", t_obs)
    must("3251", "AC5 BP test", t_bp)
    must("3251", "AC5 quota test", t_q)
    must("3251", "AC5 schedule test", t_ss)
    must("check_agent_deny_class_3251", "AC5 build.py", build)

    if "AgentRegistry" in spawn[spawn.find("Issue #3251") : spawn.find("Issue #3251") + 1600]:
        fails.append("AC5: must not introduce AgentRegistry")
    if _read("tests/orch/test_issue_3251.cpp") or _read("tests/compiler/test_issue_3251.cpp"):
        fails.append("AC5: test_issue_3251.cpp present (forbidden #81967)")
    if _read("docs/design/3251-agent-deny-class.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3251 agent_deny_class:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3251 agent_deny_class: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

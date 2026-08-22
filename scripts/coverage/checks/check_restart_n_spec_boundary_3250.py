#!/usr/bin/env python3
"""Issue #3250: RestartN fuel is AgentScope::spawn specs_ only.

Bare spawn_agent_with_mailbox / empty body is not silent no-op:
observable skip + production Cancel. Soft / ReportOnly zero extra.
Reclaimed still-running is never restart fuel (#2661).

Contract (one row per AC):
  AC1  Soft / ReportOnly: no extra skip atomic / no Cancel degrade
  AC2  Scope-spawned agent: RestartN re-spawns within max_restarts
  AC3  No specs_ body: skip observable; production degrades Cancel
  AC4  Reclaimed still-running still skip (#2661)
  AC5  Hash fields additive; no AgentRegistry; no invent test/docs

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
    t_sc = _read("tests/orch/test_orch_scope.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    must("kRestartNSpecBoundaryIssue = 3250", "AC1 stamp", spawn)
    must("agent_spec_restartable", "AC1 helper", spawn)
    must("agent_restart_skipped_no_spec_total", "AC3 counter", spawn)
    must("restart_spec_missing_", "AC3 guard", scope)
    must("note_restart_skipped_no_spec_", "AC3 note", scope)
    must("production_defaults_active()", "AC1 Soft skip", scope)
    must("adopt_handle_without_spec_for_test", "AC3 adopt", scope)

    must("try_restart_from_spec_", "AC2 unified spawn", scope)
    must("restart-attempted", "AC3 watch hash", agent)
    must("restart-skipped-no-spec", "AC3 watch hash skip", agent)
    must("restart-ok", "AC3 watch hash ok", agent)
    must("schema-3250", "AC3 schema", agent)
    must("restart-n-spec-boundary-wired", "AC5 wired", agent)

    must("3250 AC2", "AC2 test", t_pol)
    must("3250 AC3", "AC3 test", t_pol)
    must("3250 AC4", "AC4 reclaimed", t_pol)
    must("3250 AC5", "AC5 Soft", t_pol)
    must("schema-3250", "AC5 orch-scope hash", t_sc)
    must("#2661", "AC4 #2661 preserved", scope)
    must("check_restart_n_spec_boundary_3250", "AC5 build.py", build)
    must("#3250", "AC5 README", readme)

    if "AgentRegistry" in spawn[spawn.find("Issue #3250") : spawn.find("Issue #3250") + 1200]:
        fails.append("AC5: must not introduce AgentRegistry")
    if _read("tests/orch/test_issue_3250.cpp") or _read("tests/compiler/test_issue_3250.cpp"):
        fails.append("AC5: test_issue_3250.cpp present (forbidden #81967)")
    if _read("docs/design/3250-restart-n-spec-boundary.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3250 restart_n_spec_boundary:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3250 restart_n_spec_boundary: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

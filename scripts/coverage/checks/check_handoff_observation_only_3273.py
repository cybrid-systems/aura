#!/usr/bin/env python3
"""Issue #3273: make the cross-Evaluator handoff observation-only contract
explicit in types + Aura hashes (no ownership transfer, no registry).

join_via_handoff / orch:join-via-token never take ownership, never release
the source reservation, never detach the source mailbox, and never move the
name into the importer table. The typed result carries observation_only /
reservation_held_by_source and the Aura hash exposes observation-only /
ownership=source / reservation-held-by-source under production. No
process-global registry (Session-local Scope + per-Evaluator name table
remain SSOT); cross-Eval stays an explicit token pass (#3216 planes).

Contract (one row per AC):
  AC1  typed result: observation_only=true, reservation_held_by_source=true
       on every path (valid + Invalid); no release/detach surface on the type
  AC2  dual-Evaluator: importer observes, source owns reservation until
       source join_agent / dtor; importer drop does not detach source mailbox
  AC3  Aura hash: observation-only / ownership=source /
       reservation-held-by-source under production (valid + invalid)
  AC4  identity-plane stays name-table | scope-handle | directory; handoff
       is not a fourth plane; no plane merge
  AC5  no AgentRegistry / global map; stash stays g_handoff_token_stash;
       no new query:*; no test_issue_3273.cpp (#81967); no docs/design/
       (#1655); build.py wires linter

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
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    scopeh = _read("src/orch/agent_scope.h")

    must("kHandoffObservationOnlyIssue = 3273", "AC5 stamp", spawn)
    must("observation_only = true", "AC1 typed field", spawn)
    must("reservation_held_by_source = true", "AC1 typed field", spawn)
    must("add_handoff_observation_only", "AC3 Aura helper", prim)
    must("observation-only", "AC3 Aura key", prim)
    must("reservation-held-by-source", "AC3 Aura key", prim)
    must("ownership", "AC3 Aura key", prim)
    must("schema-3273", "AC3 schema stamp", prim)
    must("handoff-observation-only-wired", "AC3 wired marker", prim)
    must("3273 AC1", "AC1 test", test)
    must("3273 AC2", "AC2 test", test)
    must("3273 AC3", "AC3 test", test)
    must("3273 AC5", "AC5 test", test)
    must("importer drop keeps source mailbox", "AC2 no-detach", test)
    must("check_handoff_observation_only_3273", "AC5 build.py", build)
    if "query:handoff-observation" in prim or "query:ownership" in prim:
        fails.append("AC3: new query:* (reuse query:orch-module-stats)")
    if "release_source_reservation" in spawn or "detach_source_mailbox" in spawn:
        fails.append("AC1: release/detach surface must not exist on the type")
    if "class AgentRegistry" in spawn or "class AgentRegistry" in scopeh:
        fails.append("AC5: process-global AgentRegistry (forbidden)")
    if _read("tests/orch/test_issue_3273.cpp"):
        fails.append("AC5: test_issue_3273.cpp present (forbidden #81967)")
    if _read("docs/design/3273-handoff-observation-only.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3273 handoff_observation_only:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3273 handoff_observation_only: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

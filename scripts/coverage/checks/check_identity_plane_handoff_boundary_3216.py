#!/usr/bin/env python3
"""Issue #3216: three identity planes + observation-only HandoffToken.

Capability-boundary residual of production multi-agent orchestration:
name-table / scope-handle / directory stay separate session-local planes;
HandoffToken + join_via_handoff is read-only observation (no ownership
move, no session-spanning workflow). No process-global AgentRegistry.

Contract (one row per AC):
  AC1  Code + orch README document three planes + HandoffToken
       observe-only. No new process-global table. No orch:resolve-via-token.
  AC2  Production join / scope-resolve / directory / join-via-token hashes
       expose identity-plane (name-table | scope-handle | directory) and
       handoff-token-present so Agents distinguish name-table miss vs
       scope no handle vs handoff source still-running. schema-3216 on
       query:orch-module-stats.
  AC3  Soft / Off: add_identity_plane / add_handoff_token_present gated on
       production_defaults_active — zero extra intern on the quiet path.
  AC4  Tests extend test_join_drain_reclaim + test_orch_scope + existing
       directory_snapshot HardDeny (test_agent_scope). Linter in build.py.
       No tests/orch/test_issue_3216.cpp (#81967); no docs/design/3216-*
       (#1655).

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
    names = _read("src/compiler/agent_name_table.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    join_t = _read("tests/orch/test_join_drain_reclaim.cpp")
    orch_t = _read("tests/orch/test_orch_scope.cpp")
    scope_t = _read("tests/orch/test_agent_scope.cpp")
    build = _read("build.py")

    # ── AC1: document three planes + HandoffToken observe-only
    must("kIdentityPlaneHandoffBoundaryIssue = 3216", "AC1 issue constant", spawn)
    must("observation-only", "AC1 HandoffToken observation-only", spawn)
    must("name-table", "AC1 agent_scope name-table plane", scope)
    must("scope-handle", "AC1 agent_scope scope-handle plane", scope)
    must("directory_snapshot", "AC1 agent_scope directory plane", scope)
    must("name-table plane", "AC1 AgentNameTable plane cite", names)
    must("Identity planes and HandoffToken boundary (Issue #3216)", "AC1 README section", readme)
    must("handoff-token-present", "AC1 README handoff flag", readme)
    if "orch:resolve-via-token" in prim:
        fails.append("AC1: orch:resolve-via-token added (SlimSurface forbids new orch:* prim)")
    if "class AgentRegistry" in spawn or "struct AgentRegistry" in spawn:
        fails.append("AC1: process-global AgentRegistry type reintroduced in agent_spawn.h")
    if "g_global_agent_registry" in spawn or "global_agent_registry =" in spawn:
        fails.append("AC1: global_agent_registry symbol reintroduced in agent_spawn.h")

    # ── AC2: production hashes distinguish the three miss / observe cases
    must("add_identity_plane", "AC2 helper", prim)
    must("add_handoff_token_present", "AC2 handoff helper", prim)
    must('"name-table"', "AC2 name-table intern", prim)
    must('"scope-handle"', "AC2 scope-handle intern", prim)
    must('"directory"', "AC2 directory intern", prim)
    must("handoff-token-present", "AC2 handoff-token-present key", prim)
    must("schema-3216", "AC2 schema-3216 on module-stats", prim)
    must("identity-plane-wired", "AC2 identity-plane-wired sentinel", prim)
    must('add_identity_plane(kv, "name-table")', "AC2 join wires name-table", prim)
    must('add_identity_plane(kv, "scope-handle")', "AC2 scope-resolve wires scope-handle", prim)
    must('add_identity_plane(kv, "directory")', "AC2 directory wires directory", prim)
    if "query:identity-plane" in prim or "query:orch-identity" in prim:
        fails.append("AC2: new query:* name (reuse query:orch-module-stats)")

    # ── AC3: Soft gated on production_defaults_active
    must("production_defaults_active()", "AC3 helper gated", prim)
    # Both helpers must return before intern when Soft.
    helper_pos = prim.find("auto add_identity_plane")
    if helper_pos == -1:
        fails.append("AC3: add_identity_plane helper missing")
    else:
        block = prim[helper_pos : helper_pos + 900]
        if "production_defaults_active()" not in block:
            fails.append("AC3: add_identity_plane not gated on production_defaults_active")
        if "return;" not in block:
            fails.append("AC3: add_identity_plane must early-return on Soft")
    ht_pos = prim.find("auto add_handoff_token_present")
    if ht_pos == -1:
        fails.append("AC3: add_handoff_token_present helper missing")
    else:
        block = prim[ht_pos : ht_pos + 700]
        if "production_defaults_active()" not in block:
            fails.append("AC3: add_handoff_token_present not gated on production_defaults_active")

    # ── AC4: tests + linter wire; no invent / design doc
    must("ac3216_1", "AC4 join miss test", join_t)
    must("handoff-token-present", "AC4 join-via-token test", join_t)
    must("ac3216_3: handoff source still-running", "AC4 still-running", join_t)
    must("ac3216_4: Soft join miss has no identity-plane key", "AC4 Soft skip", join_t)
    must("ac3216_scope: miss identity-plane=scope-handle", "AC4 scope-resolve", orch_t)
    must("ac3216_dir: identity-plane=directory", "AC4 directory", orch_t)
    must("ac3216_handoff_directory_hard_deny", "AC4 HardDeny + handoff", scope_t)
    must("check_identity_plane_handoff_boundary_3216", "AC4 build.py wire", build)
    if (ROOT / "tests" / "orch" / "test_issue_3216.cpp").is_file():
        fails.append("AC4: tests/orch/test_issue_3216.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3216.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3216.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3216-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3216 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3216 identity-plane + HandoffToken boundary — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

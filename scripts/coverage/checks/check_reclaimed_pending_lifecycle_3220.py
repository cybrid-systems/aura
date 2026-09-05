#!/usr/bin/env python3
"""Issue #3220: production Timeout after auto-wait — directory / join
hash distinguish still-held vs reclaimable.

After #3146 retained must_wait_reclaimed on auto-wait Timeout, the
name-table / directory_snapshot could still list the handle as a live
name until ~AgentHandle. Agents that only read join status (not
must-wait-reclaimed) might reuse the name while reservation is held.

Contract (one row per AC):
  AC1  directory_snapshot sets lifecycle=reclaimed-pending under
       production when must_wait_reclaimed || reclaimed_deferred_cleanup;
       join hash exposes wait-reclaimed-timeout; host_forget counter
       bumps on auto-wait Timeout
  AC2  Soft: lifecycle empty / no intern (production_defaults gate);
       facade wait path still zero extra
  AC3  #2661 complete_agent_join_cleanup Reclaimed path unchanged
  AC4  test_join_drain_reclaim ac3220_*; this linter in build.py;
       no docs/design/3220-*; no test_issue_3220.cpp; no AgentRegistry;
       no new query:* (reuse query:orch-module-stats)
  AC5  Issue #3527: AgentDirectoryEntry appends reclaimed_deferred +
       must_wait_reclaimed at struct END; directory_snapshot populates
       from the handle; status=reclaimed when deferred; orch:agent-directory
       / orch:scope-resolve emit the same bool keys. No new query:*.

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
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    t = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    readme = _read("src/orch/README.md")

    must("kReclaimedPendingLifecycleIssue = 3220", "AC1 constant", spawn)
    must("host_forget_reclaimed_risk_total", "AC1 counter", spawn)
    must("reclaimed-pending", "AC1 directory token", scope)
    must("h.must_wait_reclaimed || h.reclaimed_deferred_cleanup", "AC1 directory predicate", scope)
    must("wait-reclaimed-timeout", "AC1 join hash alias", prim)
    must("add_reclaimed_pending_lifecycle", "AC1 Aura helper", prim)
    must("host-forget-reclaimed-risk-total", "AC1 query key", prim)
    must("schema-3220", "AC1 schema", prim)

    must("production_defaults_active()", "AC2 Soft gate directory", scope)
    must("if (!pending)", "AC2 helper skip empty", prim)
    must("production_defaults_active()", "AC2 helper Soft skip intern", prim)

    must("complete_agent_join_cleanup", "AC3 #2661 helper", spawn)
    must("#2661 no-early-free", "AC3 #2661 lineage", spawn)

    must("3220 AC1: directory lifecycle=reclaimed-pending", "AC4 test AC1", t)
    must("3220 AC2: Soft directory lifecycle empty", "AC4 test AC2", t)
    must("check_reclaimed_pending_lifecycle_3220", "AC4 build.py", build)
    must("lifecycle=reclaimed-pending", "AC4 README", readme)
    if "query:reclaimed-pending" in prim or "query:host-forget" in prim:
        fails.append("AC4: new query:* name (reuse query:orch-module-stats)")
    if (
        "AgentRegistry" in spawn[spawn.find("Issue #3220") : spawn.find("Issue #3220") + 800]
        if "Issue #3220" in spawn
        else ""
    ):
        fails.append("AC4: AgentRegistry in #3220 window")

    if (ROOT / "tests" / "orch" / "test_issue_3220.cpp").is_file():
        fails.append("AC4: tests/orch/test_issue_3220.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3220.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3220.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3220-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    # ── AC5: Issue #3527 three-plane Reclaimed bools ──
    must("e.reclaimed_deferred = h.reclaimed_deferred_cleanup", "AC5 directory populate", scope)
    must("e.must_wait_reclaimed = h.must_wait_reclaimed", "AC5 directory populate", scope)
    must('e.status = "reclaimed"', "AC5 directory status", scope)
    must("reclaimed-deferred", "AC5 directory hash", prim)
    must("must-wait-reclaimed", "AC5 scope-resolve hash", prim)
    must("ce.reclaimed_deferred = e.reclaimed_deferred", "AC5 cross-scope copy", scope)
    must("Issue #3527", "AC5 directory struct", scope)
    must("3527", "AC5 tests", t)
    must("Three-plane Reclaimed (#3527)", "AC5 README", readme)
    if "query:reclaimed-deferred" in prim or "query:must-wait-reclaimed" in prim:
        fails.append("AC5: new query:* name (reuse query:orch-module-stats)")
    if (ROOT / "tests" / "orch" / "test_issue_3527.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3527.cpp present (forbidden #81967)")
    if docs.is_dir():
        for f in sorted(docs.glob("3527-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3220 reclaimed_pending_lifecycle:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3220/#3527 reclaimed_pending_lifecycle: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

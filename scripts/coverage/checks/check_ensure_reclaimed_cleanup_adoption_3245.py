#!/usr/bin/env python3
"""Issue #3245: C++ long-lived hosts adopt ensure_reclaimed_cleanup.

#3110 auto-waits 50ms; #3220 bumps host_forget on Timeout. Residual:
hosts that store the handle without calling ensure only saw the join
counter. Moving a still-pending handle re-bumps the existing risk
counter. Soft / explicit wait: must_wait is false → no extra.

Contract (one row per AC):
  AC1  Soft / explicit wait_reclaimed_ms: zero extra (no hold bump)
  AC2  production Reclaimed + body exit in 50ms: must_wait=false
  AC3  production Timeout: must_wait retained; ensure second-closes;
       storing pending handle re-bumps host_forget; #2661 no early free
  AC4  name-table find during pending → reclaimed-pending (#3220)
  AC5  extend test_join_drain_reclaim; no test_issue_3245.cpp;
       no docs/design/ (#1655); no new query:* / AgentRegistry

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
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")

    must("kEnsureReclaimedCleanupAdoptionIssue = 3245", "AC5 stamp", spawn)
    must("note_reclaimed_pending_hold", "AC3 hold helper", spawn)
    must("ensure_reclaimed_cleanup", "AC3 helper", spawn)
    must("host_forget_reclaimed_risk_total", "AC3 reuse risk counter", spawn)
    must("if (!pending)", "AC1 Soft no extra", spawn)
    must("ac3245_1_soft", "AC1 test", test)
    must("ac3245_2_ok", "AC2 test", test)
    must("ac3245_3_hold", "AC3 test", test)
    must("reclaimed-pending", "AC4 pending find", test)
    must("#2661 no early free", "AC3 #2661", test)
    must("MUST call this", "AC5 contract", spawn)
    must("check_ensure_reclaimed_cleanup_adoption_3245", "AC5 build.py", build)
    if "query:ensure-reclaimed" in prim or "query:host-forget-pending" in prim:
        fails.append("AC5: new query:* (reuse query:orch-module-stats)")
    if _read("tests/orch/test_issue_3245.cpp"):
        fails.append("AC5: test_issue_3245.cpp present (forbidden #81967)")
    if _read("docs/design/3245-ensure-reclaimed-cleanup.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3245 ensure_reclaimed_cleanup_adoption:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3245 ensure_reclaimed_cleanup_adoption: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
